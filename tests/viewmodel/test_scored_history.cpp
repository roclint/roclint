#include "HostController.hpp"
#include "support/fixtures.hpp"

#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

using namespace roclint;
using namespace roclint::core;

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setOrganizationName("roclint_scored_history_test");
    app.setApplicationName("isolated");
    QTemporaryDir directory;
    ROCLINT_CHECK(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    const auto database = directory.filePath("history.sqlite");
    QString session;
    std::optional<analysis::AnalysisSnapshot> saved;
    {
        host::HostController controller(database);
    ROCLINT_CHECK(controller.waitForStorageForTesting());
        controller.setLegacyDiscoveryForTesting(true);
        controller.setScorerEngine("jev");
        controller.s1AdapterForTesting().setMockCloudScorer([](const analysis::NormalizedAnalysis& normalized)
            -> Result<bridge::SnapshotEvaluationResponse> {
            auto result = bridge::S1TriageAdapter::degradedResponse(normalized, "");
            result.modelVersion = "fixture-scorer@1";
            auto& score = result.groupScores.at(0);
            score.isScored = true;
            score.ambiguityScore = 0.8;
            score.winningAlternativeId = "alt_0_1";
            score.probabilities = std::vector<bridge::AlternativeProbability>{{"alt_0_0", 0.2}, {"alt_0_1", 0.8}};
            return result;
        });
        controller.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> {
            return std::string(R"({"status":"options_generated","choice_groups":[{"question":"Which speed?","material":true,
              "alternatives":[{"label":"Lower latency","why_material":"Single calls","evidence":["runs faster"]},
                              {"label":"Higher throughput","why_material":"Batches","evidence":["runs faster"]}]}]})");
        });
        ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
        ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(QString::fromStdString(fx::kChainPrompt), 0)));
        ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
        ROCLINT_CHECK(controller.analysisStatus() == "READY");
        ROCLINT_CHECK(controller.finalSnapshotForTesting());
        saved = controller.finalSnapshotForTesting();
        ROCLINT_CHECK(saved->evaluation.isRanked());
        ROCLINT_CHECK(controller.activeAlternatives().at(0).toMap().value("id").toString() == "alt_0_1");
        ROCLINT_CHECK(!controller.confirmedIntentForTesting());
        // The label at position 1 carries its real alternative ID after reordering.
        ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice("alt_0_1", controller.stateVersion())));
        ROCLINT_CHECK(controller.confirmedIntentForTesting()->snapshotId == saved->id);
        session = controller.currentSessionIdString();
    }
    {
        host::HostController controller(database);
    ROCLINT_CHECK(controller.waitForStorageForTesting());
        controller.setLegacyDiscoveryForTesting(true);
        // Neither a configured scorer nor a live Granite model is available on restore.
        controller.setScorerEngine("julia");
        ROCLINT_CHECK(controller.waitForCommandForTesting(controller.restoreSession(session, controller.stateVersion())));
        ROCLINT_CHECK(controller.finalSnapshotForTesting() == saved);
        ROCLINT_CHECK(controller.confirmedIntentForTesting());
        ROCLINT_CHECK(controller.confirmedIntentForTesting()->snapshotHash == saved->snapshotHash);
        ROCLINT_CHECK(controller.confirmedIntentForTesting()->items.at(0).chosenAlternativeId == "alt_0_1");
        ROCLINT_CHECK(!controller.isIntentStale());
        const auto restored = controller.repositoryForTesting()->restoreSession(SessionId(session.toStdString())).waitForTesting();
        ROCLINT_CHECK(restored.ok() && restored.value().analysis == saved);
    }
    std::cout << "Controller persists scored ordering and restores its exact source binding without rescoring.\n";
}
