/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QThread>

#include <functional>
#include <memory>

#include "engine/Engine.h"
#include "ui/EngineBridge.h"
#include "ui/MainWindow.h"

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

bool waitFor(const std::function<bool()>& done, int timeoutMs = 3000) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 10) {
        if (done()) return true;
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return done();
}

}  // namespace

TEST_CASE("input picker assigns and clears sources") {
    (void)application();

    Engine engine;
    EngineConfig cfg;
    cfg.ndiOut = false;
    cfg.audio = false;
    cfg.inputs = {{InputSpec::Type::Ndi, ""}, {InputSpec::Type::Ndi, ""}};
    REQUIRE(engine.start(cfg));

    {
        EngineBridge bridge(engine);
        MainWindow window(bridge, {QString(), QString()});

        const auto pickers =
            window.findChildren<QComboBox*>(QStringLiteral("inputPicker"));
        REQUIRE(pickers.size() == 2);
        QComboBox* picker = pickers[0];

        // Closed pickers show only the current assignment.
        CHECK(picker->count() == 1);
        CHECK(picker->currentText() == QStringLiteral("BLACK"));

        // Opening rebuilds from live discovery: BLACK heads the list, the
        // manual SRT entry ends it, and an unassigned input selects BLACK.
        picker->showPopup();
        picker->hidePopup();
        REQUIRE(picker->count() >= 2);
        CHECK(picker->itemText(0) == QStringLiteral("BLACK"));
        CHECK(picker->itemText(picker->count() - 1) ==
              QStringLiteral("SRT URL…"));
        CHECK(picker->currentIndex() == 0);

        // Activating a (synthetic) discovery row patches the engine input.
        picker->addItem(QStringLiteral("NDI · MooCam"),
                        int(InputSpec::Type::Ndi));
        picker->setItemData(picker->count() - 1, QStringLiteral("MooCam"),
                            Qt::UserRole + 1);
        emit picker->activated(picker->count() - 1);
        REQUIRE(waitFor([&] { return engine.inputRef(0) == "MooCam"; }));
        CHECK(engine.inputType(0) == InputSpec::Type::Ndi);

        // The bridge poll collapses the picker to the new assignment.
        REQUIRE(waitFor([&] {
            return picker->count() == 1 &&
                   picker->currentText() == QStringLiteral("MooCam");
        }));

        // Reopening keeps the offline assignment visible and selected while
        // BLACK stays first; picking BLACK unassigns the input again.
        picker->showPopup();
        picker->hidePopup();
        CHECK(picker->currentText() == QStringLiteral("MooCam"));
        CHECK(picker->itemText(0) == QStringLiteral("BLACK"));
        emit picker->activated(0);
        REQUIRE(waitFor([&] { return engine.inputRef(0).empty(); }));
    }

    // Replaced inputs are destroyed on detached threads; give them time to
    // finish before process teardown tears the runtime out from under them.
    QThread::msleep(400);
    engine.stop();
}
