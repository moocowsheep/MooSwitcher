#include <algorithm>

#include <QApplication>
#include <QIcon>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>

#include "core/Log.h"
#include "engine/Engine.h"
#include "ui/EngineBridge.h"
#include "ui/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MooSwitcher"));
    QApplication::setWindowIcon(
        QIcon(QStringLiteral(":/branding/cow-switcher-logo.svg")));

    // The show file loads first; CLI flags override what they name.
    QString showFilePath;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size() - 1; ++i)
        if (args[i] == QStringLiteral("--show-file")) showFilePath = args[i + 1];
    moo::ui::ShowFile showFile(showFilePath);
    moo::ui::ShowFile::State show;
    if (showFile.load(show))
        MOO_LOGI("show restored from %s", showFile.path().toUtf8().constData());

    moo::EngineConfig& cfg = show.cfg;
    QString shotPath;
    QString recordPath;
    double shotDelayS = 6.0;
    int shotTab = 0;
    bool cliInputs = false;
    auto addInput = [&](moo::InputSpec::Type t, const QString& ref) {
        if (!cliInputs) cfg.inputs.clear();  // CLI replaces the stored set
        cliInputs = true;
        cfg.inputs.push_back({t, ref.toStdString()});
    };
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--input") && i + 1 < args.size())
            addInput(moo::InputSpec::Type::Ndi, args[++i]);
        else if (args[i] == QStringLiteral("--srt-input") && i + 1 < args.size())
            addInput(moo::InputSpec::Type::Srt, args[++i]);
        else if (args[i] == QStringLiteral("--media-input") &&
                 i + 1 < args.size())
            addInput(moo::InputSpec::Type::Media, args[++i]);
        else if (args[i] == QStringLiteral("--validate"))
            cfg.validation = true;
        else if (args[i] == QStringLiteral("--show") && i + 1 < args.size()) {
            const auto parts = args[++i].split('x');
            if (parts.size() == 2) {
                cfg.show.width = parts[0].toInt();
                cfg.show.height = parts[1].toInt();
            }
        } else if (args[i] == QStringLiteral("--srt-out") && i + 1 < args.size())
            cfg.srtUrl = args[++i].toStdString();
        else if (args[i] == QStringLiteral("--srt-bitrate") && i + 1 < args.size())
            cfg.srtBitrateKbps = args[++i].toInt();
        else if (args[i] == QStringLiteral("--record") && i + 1 < args.size())
            recordPath = args[++i];
        else if (args[i] == QStringLiteral("--record-bitrate") &&
                 i + 1 < args.size())
            cfg.recordBitrateKbps = args[++i].toInt();
        else if (args[i] == QStringLiteral("--show-file"))
            ++i;  // consumed above
        else if (args[i] == QStringLiteral("--screenshot") && i + 1 < args.size())
            shotPath = args[++i];  // grab the window after --shot-delay, quit
        else if (args[i] == QStringLiteral("--shot-delay") && i + 1 < args.size())
            shotDelayS = args[++i].toDouble();
        else if (args[i] == QStringLiteral("--screenshot-tab") &&
                 i + 1 < args.size())
            shotTab = args[++i].toInt();
    }
    if (cfg.inputs.empty())
        cfg.inputs = {{moo::InputSpec::Type::Ndi, "MooBenchA"},
                      {moo::InputSpec::Type::Ndi, "MooBenchB"}};
    show.chans.resize(cfg.inputs.size());

    moo::Engine engine;
    if (!engine.start(cfg)) {
        MOO_LOGE("engine start failed");
        return 1;
    }
    if (!recordPath.isEmpty())
        engine.requestRecording(recordPath.toStdString());

    // Control-surface and mixer state restore.
    if (auto* aud = engine.audio())
        for (int i = 0; i < aud->inputCount() && i < int(show.chans.size()); ++i) {
            const auto& ch = show.chans[size_t(i)];
            aud->channel(i).gain.store(ch.gain);
            aud->channel(i).mute.store(ch.mute);
            aud->channel(i).solo.store(ch.solo);
            aud->channel(i).delayMs.store(ch.delayMs);
        }

    QStringList names;
    for (const auto& n : cfg.inputs) names << QString::fromStdString(n.ref);

    moo::ui::EngineBridge bridge(engine);
    moo::ui::MainWindow win(bridge, names, &showFile, &show);
    QObject::connect(&app, &QApplication::aboutToQuit, &win,
                     &moo::ui::MainWindow::saveShow);
    win.show();

    if (!shotPath.isEmpty()) {
        if (auto* tabs = win.findChild<QTabWidget*>(
                QStringLiteral("workspaceTabs")))
            tabs->setCurrentIndex(std::clamp(shotTab, 0, tabs->count() - 1));
        // Self-capture for verification: compositor screenshots lie about
        // occluded/unfocused Wayland windows (stale first buffer).
        QTimer::singleShot(int(shotDelayS * 1000), &win, [&win, &app, shotPath] {
            const bool ok = win.grab().save(shotPath);
            MOO_LOGI("screenshot %s: %s", ok ? "saved" : "FAILED",
                     shotPath.toUtf8().constData());
            app.quit();
        });
    }

    const int rc = app.exec();
    engine.stop();
    return rc;
}
