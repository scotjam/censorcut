#include "core/TrustLedger.h"

#include <QObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace censorcut;

class TestTrustLedger : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Redirect QStandardPaths::HomeLocation to a per-test temp dir so
        // the ledger's ~/.censorcut/trust.json doesn't touch the real
        // home directory. QStandardPaths::setTestModeEnabled rewrites
        // every standard path to a temp prefix.
        QStandardPaths::setTestModeEnabled(true);
    }

    void unseenAuthorIsAtFloor()
    {
        TrustLedger l;
        l.reset();
        QCOMPARE(l.weightFor(QStringLiteral("aabbcc")), 0.1);
    }

    void rewardThenWeightReflectsIt()
    {
        TrustLedger l;
        l.reset();
        const QString k = QStringLiteral("deadbeef");
        l.rewardAuthor(k);  // 0.1 + 0.05 = 0.15
        QCOMPARE(l.weightFor(k), 0.15);
        l.rewardAuthor(k);  // 0.20
        l.rewardAuthor(k);  // 0.25
        QVERIFY(qFuzzyCompare(l.weightFor(k), 0.25));
    }

    void penalizeClampsAtZero()
    {
        TrustLedger l;
        l.reset();
        const QString k = QStringLiteral("badpeer");
        for (int i = 0; i < 50; ++i) l.penalizeAuthor(k);
        QCOMPARE(l.weightFor(k), 0.0);
    }

    void rewardClampsAtCap()
    {
        TrustLedger l;
        l.reset();
        const QString k = QStringLiteral("greatpeer");
        for (int i = 0; i < 1000; ++i) l.rewardAuthor(k);
        QCOMPARE(l.weightFor(k), TrustLedger::kCap);
    }

    void outboundEndorsementsOnlyHighTrust()
    {
        TrustLedger l;
        l.reset();
        const QString hi  = QStringLiteral("highkey");
        const QString lo  = QStringLiteral("lowkey");
        // hi: bring up to 0.85
        for (int i = 0; i < 15; ++i) l.rewardAuthor(hi);
        // lo: keep below 0.8
        for (int i = 0; i < 5; ++i) l.rewardAuthor(lo);  // 0.1 + 5*0.05 = 0.35

        const auto out = l.outboundEndorsements();
        QVERIFY(out.contains(hi));
        QVERIFY(!out.contains(lo));
        QVERIFY(out[hi] >= TrustLedger::kPublishCutoff);
    }

    void transitiveBootstrapDepth2()
    {
        // Setup: direct trust in A = 0.8 (the publish cutoff, reached
        // by 14 reward ticks: 0.1 + 14 × 0.05 = 0.8).
        // A endorses B at 0.95. B endorses C at 0.9. C is unseen by me.
        // Expected: weightFor(C) = floor(0.1) + 0.5^2 × 0.8 × 0.95 × 0.9
        //                        = 0.1 + 0.25 × 0.8 × 0.95 × 0.9
        //                        = 0.1 + 0.171
        //                        = 0.271
        TrustLedger l;
        l.reset();
        const QString A = QStringLiteral("aaaaaaaa");
        const QString B = QStringLiteral("bbbbbbbb");
        const QString C = QStringLiteral("cccccccc");
        for (int i = 0; i < 14; ++i) l.rewardAuthor(A);
        QVERIFY(l.weightFor(A) >= 0.8 - 1e-9);
        QHash<QString, double> aE; aE.insert(B, 0.95);
        QHash<QString, double> bE; bE.insert(C, 0.9);
        l.setEndorsementsFrom(A, aE);
        l.setEndorsementsFrom(B, bE);

        const double w = l.weightFor(C);
        QVERIFY2(qAbs(w - 0.271) < 0.001,
                 qPrintable(QStringLiteral("expected ~0.271, got %1").arg(w)));
    }

    void bootstrapCappedAt04()
    {
        // Many strong chains should not push bootstrap above 0.4.
        TrustLedger l;
        l.reset();
        const QString me_seed_prefix = QStringLiteral("seed");
        const QString target = QStringLiteral("ttttgoal");
        // Create 30 directly-trusted seeds, each endorsing target at 1.0.
        for (int i = 0; i < 30; ++i) {
            const QString seed = me_seed_prefix + QString::number(i);
            for (int j = 0; j < 14; ++j) l.rewardAuthor(seed);
            QHash<QString, double> e; e.insert(target, 1.0);
            l.setEndorsementsFrom(seed, e);
        }
        QVERIFY(l.weightFor(target) <= 0.4 + 1e-9);
    }

    void promotionInheritsBootstrap()
    {
        // A peer who was reachable transitively (e.g. 0.4) and is then
        // rewarded for their first useful suggestion should NOT regress
        // to floor + delta (= 0.15). The promotion should seed direct
        // trust from the bootstrap value so the graduation isn't a
        // punishment.
        TrustLedger l;
        l.reset();
        const QString seed = QStringLiteral("seedy");
        const QString tgt  = QStringLiteral("graduate");
        // Seed must be at >= 0.8 to participate in bootstrap walk.
        for (int i = 0; i < 14; ++i) l.rewardAuthor(seed);
        QHash<QString, double> e; e.insert(tgt, 1.0);
        l.setEndorsementsFrom(seed, e);
        // tgt's bootstrap floor: 0.1 + 0.5 × 0.8 × 1.0 = 0.5 → capped at 0.4.
        QCOMPARE(l.weightFor(tgt), 0.4);

        // First reward: should inherit bootstrap (0.4) and add accept
        // delta → 0.45.
        l.rewardAuthor(tgt);
        QVERIFY2(qAbs(l.weightFor(tgt) - 0.45) < 1e-9,
                 qPrintable(QStringLiteral("expected 0.45, got %1").arg(l.weightFor(tgt))));
    }

    void promotionByPenaltyAlsoInheritsBootstrap()
    {
        // Same inheritance rule on penalty: a transitively-reachable
        // peer who turns out bad shouldn't get a free pass either —
        // we drop the inherited bootstrap by the penalty delta.
        TrustLedger l;
        l.reset();
        const QString seed = QStringLiteral("seedy");
        const QString tgt  = QStringLiteral("badgrad");
        for (int i = 0; i < 14; ++i) l.rewardAuthor(seed);
        QHash<QString, double> e; e.insert(tgt, 1.0);
        l.setEndorsementsFrom(seed, e);
        l.penalizeAuthor(tgt);  // 0.4 - 0.08 = 0.32
        QVERIFY2(qAbs(l.weightFor(tgt) - 0.32) < 1e-9,
                 qPrintable(QStringLiteral("expected 0.32, got %1").arg(l.weightFor(tgt))));
    }

    void unrelatedDirectTrustGetsNoBootstrapBoost()
    {
        // A peer with no incoming endorsements still seeds at floor on
        // first reward (no bootstrap path means inheritance is 0).
        TrustLedger l;
        l.reset();
        const QString k = QStringLiteral("isolated");
        l.rewardAuthor(k);
        QCOMPARE(l.weightFor(k), 0.15);  // 0.1 + 0.05
    }

    void directTrustDominatesBootstrapAfterPromotion()
    {
        // After promotion, new endorsement chains no longer affect a
        // peer's weight — direct experience takes over.
        TrustLedger l;
        l.reset();
        const QString seedA = QStringLiteral("seedAA");
        const QString seedB = QStringLiteral("seedBB");
        const QString tgt   = QStringLiteral("graduated2");
        for (int i = 0; i < 14; ++i) l.rewardAuthor(seedA);
        QHash<QString, double> eA; eA.insert(tgt, 1.0);
        l.setEndorsementsFrom(seedA, eA);
        l.rewardAuthor(tgt);  // inherits 0.4, climbs to 0.45
        QVERIFY2(qAbs(l.weightFor(tgt) - 0.45) < 1e-9,
                 qPrintable(QStringLiteral("expected 0.45, got %1").arg(l.weightFor(tgt))));

        // Add a second strong seed endorsing tgt. If tgt were unseen
        // this would push bootstrap higher, but tgt is direct now.
        for (int i = 0; i < 14; ++i) l.rewardAuthor(seedB);
        QHash<QString, double> eB; eB.insert(tgt, 1.0);
        l.setEndorsementsFrom(seedB, eB);
        QVERIFY2(qAbs(l.weightFor(tgt) - 0.45) < 1e-9,
                 qPrintable(QStringLiteral("expected still 0.45, got %1").arg(l.weightFor(tgt))));
    }

    void resetWipesEverything()
    {
        TrustLedger l;
        l.reset();
        const QString k = QStringLiteral("somekey");
        for (int i = 0; i < 5; ++i) l.rewardAuthor(k);
        QVERIFY(l.weightFor(k) > 0.1);
        l.reset();
        QCOMPARE(l.weightFor(k), 0.1);
    }

    void cleanupTestCase()
    {
        QStandardPaths::setTestModeEnabled(false);
    }
};

QTEST_MAIN(TestTrustLedger)
#include "test_trust_ledger.moc"
