//! Eclipse Core AI engine: DeepFilterNet 3 neural speech enhancement with
//! host-rate resampling, constant latency and panic isolation (never crashes the host).

use std::collections::VecDeque;
use std::panic::{catch_unwind, AssertUnwindSafe};

use df::tract::{DfParams, DfTract, ReduceMask, RuntimeParams};
use ndarray::prelude::*;
use rubato::{FftFixedInOut, Resampler};

const MODEL_SR: usize = 48_000;

pub struct EclipseAI {
    model: DfTract,
    hop: usize,
    host_sr: usize,
    up: Option<FftFixedInOut<f32>>,
    down: Option<FftFixedInOut<f32>>,
    fifo_in: VecDeque<f32>,
    fifo_48: VecDeque<f32>,
    fifo_out48: VecDeque<f32>,
    fifo_out: VecDeque<f32>,
    // fallback delay line (same latency) used if the model ever fails
    dry: VecDeque<f32>,
    latency: usize,
    failed: bool,
    frame_in: Vec<f32>,
    frame_out: Vec<f32>,
    rs_in: Vec<Vec<f32>>,
    rs_out: Vec<Vec<f32>>,
}

impl EclipseAI {
    fn new(sample_rate: f64, max_block: usize) -> anyhow_free::Result<Self> {
        let host_sr = sample_rate.round() as usize;
        let rp = RuntimeParams::new(1, 0.0, 100.0, -15.0, 35.0, 35.0, ReduceMask::MAX);
        let model = DfTract::new(DfParams::default(), &rp).map_err(|e| e.to_string())?;
        let hop = model.hop_size;
        let model_delay = model.fft_size - hop + model.lookahead * hop;

        let (up, down) = if host_sr == MODEL_SR {
            (None, None)
        } else {
            let u = FftFixedInOut::<f32>::new(host_sr, MODEL_SR, 512, 1).map_err(|e| e.to_string())?;
            let d = FftFixedInOut::<f32>::new(MODEL_SR, host_sr, 512, 1).map_err(|e| e.to_string())?;
            (Some(u), Some(d))
        };

        let ratio = host_sr as f64 / MODEL_SR as f64;
        let (ci, cd48, up_d48, down_d) = match (&up, &down) {
            (Some(u), Some(d)) => (u.input_frames_max(), d.input_frames_max(), u.output_delay(), d.output_delay()),
            _ => (0, 0, 0, 0),
        };
        // Pad the 48 kHz path so the total delay is a whole number of host samples
        // (keeps the AMBIENT / delta null sample-exact at 44.1 kHz too).
        let g = gcd(host_sr, MODEL_SR);
        let q = MODEL_SR / g;
        let pad48 = (q - (up_d48 + model_delay) % q) % q;
        // Samples that can be waiting inside the pipeline at most -> pre-roll so a pop never underruns.
        let prefill = ci + ((hop + cd48) as f64 * ratio).ceil() as usize + 4;
        let latency = prefill + (up_d48 + model_delay + pad48) * host_sr / MODEL_SR + down_d;
        let _ = max_block;

        let mut s = EclipseAI {
            model, hop, host_sr, up, down,
            fifo_in: VecDeque::with_capacity(8192),
            fifo_48: VecDeque::with_capacity(8192),
            fifo_out48: VecDeque::with_capacity(8192),
            fifo_out: VecDeque::with_capacity(prefill + 16384),
            dry: VecDeque::with_capacity(latency + 16384),
            latency, failed: false,
            frame_in: vec![0.0; hop], frame_out: vec![0.0; hop],
            rs_in: vec![vec![0.0; 4096]], rs_out: vec![vec![0.0; 4096]],
        };
        s.fifo_out.extend(std::iter::repeat(0.0).take(prefill));
        s.fifo_48.extend(std::iter::repeat(0.0).take(pad48));
        s.dry.extend(std::iter::repeat(0.0).take(latency));
        Ok(s)
    }

    fn pump(&mut self) -> Result<(), String> {
        // host rate -> 48 kHz
        match self.up.as_mut() {
            None => self.fifo_48.extend(self.fifo_in.drain(..)),
            Some(u) => loop {
                let need = u.input_frames_next();
                if self.fifo_in.len() < need { break; }
                self.rs_in[0].clear();
                self.rs_in[0].extend(self.fifo_in.drain(..need));
                let out_n = u.output_frames_next();
                self.rs_out[0].resize(out_n, 0.0);
                u.process_into_buffer(&self.rs_in, &mut self.rs_out, None).map_err(|e| e.to_string())?;
                self.fifo_48.extend(self.rs_out[0].iter().copied());
            },
        }
        // neural network, one 10 ms hop at a time
        while self.fifo_48.len() >= self.hop {
            for (d, s) in self.frame_in.iter_mut().zip(self.fifo_48.drain(..self.hop)) { *d = s; }
            let input = ArrayView2::from_shape((1, self.hop), &self.frame_in).unwrap();
            let output = ArrayViewMut2::from_shape((1, self.hop), &mut self.frame_out).unwrap();
            self.model.process(input, output).map_err(|e| e.to_string())?;
            self.fifo_out48.extend(self.frame_out.iter().copied());
        }
        // 48 kHz -> host rate
        match self.down.as_mut() {
            None => self.fifo_out.extend(self.fifo_out48.drain(..)),
            Some(d) => loop {
                let need = d.input_frames_next();
                if self.fifo_out48.len() < need { break; }
                self.rs_in[0].clear();
                self.rs_in[0].extend(self.fifo_out48.drain(..need));
                let out_n = d.output_frames_next();
                self.rs_out[0].resize(out_n, 0.0);
                d.process_into_buffer(&self.rs_in, &mut self.rs_out, None).map_err(|e| e.to_string())?;
                self.fifo_out.extend(self.rs_out[0].iter().copied());
            },
        }
        Ok(())
    }

    fn process(&mut self, input: &[f32], output: &mut [f32]) -> i32 {
        self.dry.extend(input.iter().copied());
        if !self.failed {
            self.fifo_in.extend(input.iter().copied());
            let ok = catch_unwind(AssertUnwindSafe(|| self.pump())).map(|r| r.is_ok()).unwrap_or(false);
            if !ok { self.failed = true; }
        }
        for o in output.iter_mut() {
            let d = self.dry.pop_front().unwrap_or(0.0);
            *o = if self.failed { d } else { self.fifo_out.pop_front().unwrap_or(0.0) };
        }
        if self.failed { -1 } else { 0 }
    }
}

fn gcd(a: usize, b: usize) -> usize { if b == 0 { a } else { gcd(b, a % b) } }

mod anyhow_free { pub type Result<T> = std::result::Result<T, String>; }

#[no_mangle]
pub extern "C" fn eclipse_ai_create(sample_rate: f64, max_block: i32) -> *mut EclipseAI {
    let r = catch_unwind(|| EclipseAI::new(sample_rate, max_block.max(1) as usize));
    match r {
        Ok(Ok(s)) => Box::into_raw(Box::new(s)),
        _ => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn eclipse_ai_free(st: *mut EclipseAI) {
    if !st.is_null() { drop(Box::from_raw(st)); }
}

#[no_mangle]
pub unsafe extern "C" fn eclipse_ai_latency(st: *const EclipseAI) -> i32 {
    st.as_ref().map(|s| s.latency as i32).unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn eclipse_ai_set_reduction_db(st: *mut EclipseAI, db: f32) {
    if let Some(s) = st.as_mut() {
        let db = db.clamp(0.0, 100.0);
        let _ = catch_unwind(AssertUnwindSafe(|| s.model.set_atten_lim(db)));
    }
}

#[no_mangle]
pub unsafe extern "C" fn eclipse_ai_set_post_filter(st: *mut EclipseAI, beta: f32) {
    if let Some(s) = st.as_mut() {
        let b = beta.clamp(0.0, 0.05);
        let _ = catch_unwind(AssertUnwindSafe(|| s.model.set_pf_beta(b)));
    }
}

#[no_mangle]
pub unsafe extern "C" fn eclipse_ai_process(st: *mut EclipseAI, input: *const f32, output: *mut f32, n: i32) -> i32 {
    match st.as_mut() {
        Some(s) if n > 0 && !input.is_null() && !output.is_null() => {
            let i = std::slice::from_raw_parts(input, n as usize);
            let o = std::slice::from_raw_parts_mut(output, n as usize);
            s.process(i, o)
        }
        _ => -1,
    }
}

#[allow(dead_code)]
fn _unused(s: &EclipseAI) -> usize { s.host_sr }
