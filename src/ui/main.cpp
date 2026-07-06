#include <QApplication>
#include <QStringList>

#include "core/Log.h"
#include "engine/Engine.h"
#include "ui/EngineBridge.h"
#include "ui/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    moo::EngineConfig cfg;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--input") && i + 1 < args.size())
            cfg.inputs.push_back(args[++i].toStdString());
        else if (args[i] == QStringLiteral("--validate"))
            cfg.validation = true;
        else if (args[i] == QStringLiteral("--show") && i + 1 < args.size()) {
            const auto parts = args[++i].split('x');
            if (parts.size() == 2) {
                cfg.show.width = parts[0].toInt();
                cfg.show.height = parts[1].toInt();
            }
        }
    }
    if (cfg.inputs.empty()) cfg.inputs = {"MooBenchA", "MooBenchB"};

    moo::Engine engine;
    if (!engine.start(cfg)) {
        MOO_LOGE("engine start failed");
        return 1;
    }

    QStringList names;
    for (const auto& n : cfg.inputs) names << QString::fromStdString(n);

    moo::ui::EngineBridge bridge(engine);
    moo::ui::MainWindow win(bridge, names);
    win.show();

    const int rc = app.exec();
    engine.stop();
    return rc;
}
