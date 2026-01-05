void RootManagersExample()
{
    ParamManager pm;
    pm.Register<int>("answer", 42, "example");
    pm.Register<std::string>("mode", "fast", "example");
    pm.Set<std::string>("mode", "fast");
    std::cout << pm.DumpJSON(2) << std::endl;

    TTree t("t", "t");
    double x = 0.0;
    t.Branch("x", &x);
    for (int i = 0; i < 10; ++i)
    {
        x = i * 0.1;
        t.Fill();
    }
    AnalysisManager::WriteInputConfig(&t, "input_example.yaml", {"dummy.root"});

    auto *h = new TH1D("h", "Example", 10, 0, 1);
    for (int i = 0; i < 100; ++i) h->Fill(gRandom->Rndm());

    PlotManager plot;
    PlotSpec spec = PlotSpec::Simple().X("x").Y("Events").Overlay({
        OverlaySpec::Hist(h, "example", ColorSpec(1, 0, 1)),
    });
    auto *c = plot.Draw(spec, "c_example");
    c->Update();
}
