#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>

#include <memory>

#include "engine/Engine.h"
#include "ui/EngineBridge.h"
#include "ui/MainWindow.h"
#include "ui/ShowFile.h"

using namespace moo;
using namespace moo::ui;

namespace {

QApplication& application() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static int argc = 1;
    static char name[] = "moo-ui-tests";
    static char* argv[] = {name, nullptr};
    static auto app = std::make_unique<QApplication>(argc, argv);
    return *app;
}

int findResolution(const QComboBox& combo, int width, int height) {
    for (int i = 0; i < combo.count(); ++i)
        if (combo.itemData(i).toInt() == width &&
            combo.itemData(i, Qt::UserRole + 1).toInt() == height)
            return i;
    return -1;
}

int findFrameRate(const QComboBox& combo, qlonglong numerator,
                  qlonglong denominator) {
    for (int i = 0; i < combo.count(); ++i)
        if (combo.itemData(i).toLongLong() == numerator &&
            combo.itemData(i, Qt::UserRole + 1).toLongLong() == denominator)
            return i;
    return -1;
}

}  // namespace

TEST_CASE("show file preserves the exact output format") {
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());

    ShowFile file(temporary.filePath(QStringLiteral("show.ini")));
    ShowFile::State saved;
    saved.cfg.show.width = 4096;
    saved.cfg.show.height = 2160;
    saved.cfg.show.fpsN = 24000;
    saved.cfg.show.fpsD = 1001;
    file.save(saved);

    ShowFile::State restored;
    REQUIRE(file.load(restored));
    CHECK(restored.cfg.show.width == 4096);
    CHECK(restored.cfg.show.height == 2160);
    CHECK(restored.cfg.show.fpsN == 24000);
    CHECK(restored.cfg.show.fpsD == 1001);
}

TEST_CASE("output format badge tracks and saves pending selections") {
    (void)application();

    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    ShowFile file(temporary.filePath(QStringLiteral("show.ini")));

    Engine engine;
    EngineBridge bridge(engine);
    ShowFile::State initial;
    initial.cfg.show = engine.outputFormat();
    MainWindow window(bridge, {}, &file, &initial);

    auto* resolution =
        window.findChild<QComboBox*>(QStringLiteral("outputResolution"));
    auto* frameRate =
        window.findChild<QComboBox*>(QStringLiteral("outputFrameRate"));
    auto* badge =
        window.findChild<QLabel*>(QStringLiteral("formatState"));
    REQUIRE(resolution);
    REQUIRE(frameRate);
    REQUIRE(badge);

    CHECK(badge->text() == QStringLiteral("ACTIVE"));
    CHECK(badge->property("state").toString() == QStringLiteral("active"));

    const int resolutionIndex = findResolution(*resolution, 3840, 2160);
    const int frameRateIndex = findFrameRate(*frameRate, 30000, 1001);
    REQUIRE(resolutionIndex >= 0);
    REQUIRE(frameRateIndex >= 0);
    resolution->setCurrentIndex(resolutionIndex);
    frameRate->setCurrentIndex(frameRateIndex);

    CHECK(badge->text() == QStringLiteral("RESTART TO APPLY"));
    CHECK(badge->property("state").toString() == QStringLiteral("pending"));

    ShowFile::State restored;
    REQUIRE(file.load(restored));
    CHECK(restored.cfg.show.width == 3840);
    CHECK(restored.cfg.show.height == 2160);
    CHECK(restored.cfg.show.fpsN == 30000);
    CHECK(restored.cfg.show.fpsD == 1001);

    resolution->setCurrentIndex(
        findResolution(*resolution, initial.cfg.show.width,
                       initial.cfg.show.height));
    frameRate->setCurrentIndex(
        findFrameRate(*frameRate, initial.cfg.show.fpsN,
                      initial.cfg.show.fpsD));
    CHECK(badge->text() == QStringLiteral("ACTIVE"));
    CHECK(badge->property("state").toString() == QStringLiteral("active"));
}
