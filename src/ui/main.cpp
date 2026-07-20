/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QIcon>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>

#include "core/Log.h"
#include "ctl/ControlServer.h"
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
    QString cleanRecordPath;
    double shotDelayS = 6.0;
    int shotTab = 0;
    // Remote control (Companion et al). On by default: a production console
    // is expected to be reachable; 0 disables.
    int controlPort = 9923;
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
        else if (args[i] == QStringLiteral("--still-input") &&
                 i + 1 < args.size())
            addInput(moo::InputSpec::Type::Still, args[++i]);
        else if (args[i] == QStringLiteral("--validate"))
            cfg.validation = true;
        else if (args[i] == QStringLiteral("--clean-ndi-out") &&
                 i + 1 < args.size()) {
            cfg.cleanNdiOut = true;
            cfg.cleanNdiOutName = args[++i].toStdString();
        }
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
        else if (args[i] == QStringLiteral("--clean-record") &&
                 i + 1 < args.size())
            cleanRecordPath = args[++i];
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
        else if (args[i] == QStringLiteral("--control-port") &&
                 i + 1 < args.size())
            controlPort = args[++i].toInt();
    }
    // A fixed 21-input frame (7 x 3 on the multiview). Unassigned slots are
    // black until the operator patches a source from the INPUTS tab.
    constexpr size_t kInputSlots = 21;
    while (cfg.inputs.size() < kInputSlots)
        cfg.inputs.push_back({moo::InputSpec::Type::Ndi, ""});
    show.chans.resize(cfg.inputs.size());

    moo::Engine engine;
    if (!engine.start(cfg)) {
        MOO_LOGE("engine start failed");
        return 1;
    }
    if (!recordPath.isEmpty())
        engine.requestRecording(recordPath.toStdString());
    if (!cleanRecordPath.isEmpty())
        engine.requestCleanRecording(cleanRecordPath.toStdString());

    // Control-surface and mixer state restore.
    if (auto* aud = engine.audio())
        for (int i = 0; i < aud->inputCount() && i < int(show.chans.size()); ++i) {
            const auto& ch = show.chans[size_t(i)];
            aud->channel(i).gain.store(ch.gain);
            aud->channel(i).mute.store(ch.mute);
            aud->channel(i).solo.store(ch.solo);
            aud->channel(i).delayMs.store(ch.delayMs);
        }

    std::unique_ptr<moo::ctl::ControlServer> control;
    if (controlPort > 0)
        control =
            std::make_unique<moo::ctl::ControlServer>(engine, controlPort);

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
    control.reset();  // its poll thread reads engine state; stop it first
    engine.stop();
    return rc;
}
