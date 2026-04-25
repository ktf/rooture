// bench_method_dispatch.C
//
// ROOT macro equivalent of bench_method_dispatch.rut.
// Measures the per-call overhead of the same three method-call patterns
// using direct C++ calls — no interpreter overhead, no rooture dispatch.
// This gives the baseline "native C++" cost to compare against.
//
// Usage (in a ROOT session):
//   .x examples/bench_method_dispatch.C
//
// Or from the shell:
//   root -b -q examples/bench_method_dispatch.C

void bench_method_dispatch() {
    TH1F*      h  = new TH1F("bench_h", "bench", 100, -5., 5.);
    TStopwatch sw;

    h->FillRandom("gaus", 10000);

    // ── single-arg integer → Double_t return ────────────────────────────────
    sw.Start(true);
    for (int i = 0; i < 500; i++) {
        (void)h->GetBinContent(1);
    }
    sw.Stop();
    printf("500x GetBinContent(1)    : %.6f s  (%.3f us/call)\n",
           sw.RealTime(), sw.RealTime() / 500. * 1e6);

    // ── no-arg → Double_t return ─────────────────────────────────────────────
    sw.Start(true);
    for (int i = 0; i < 500; i++) {
        (void)h->GetRandom();
    }
    sw.Stop();
    printf("500x GetRandom()         : %.6f s  (%.3f us/call)\n",
           sw.RealTime(), sw.RealTime() / 500. * 1e6);

    // ── int + double → void ──────────────────────────────────────────────────
    sw.Start(true);
    for (int i = 0; i < 500; i++) {
        h->SetBinContent(1, 0.5);
    }
    sw.Stop();
    printf("500x SetBinContent(1,0.5): %.6f s  (%.3f us/call)\n",
           sw.RealTime(), sw.RealTime() / 500. * 1e6);

    delete h;
}
