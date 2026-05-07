#include "TrustLedger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace censorcut {

namespace {

double clamp01_2(double v)
{
    if (v < 0.0) return 0.0;
    if (v > TrustLedger::kCap) return TrustLedger::kCap;
    return v;
}

} // namespace

TrustLedger::TrustLedger(QObject* parent)
    : QObject(parent)
{
    load();
}

QString TrustLedger::filePath() const
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return QDir::cleanPath(home + QStringLiteral("/.censorcut/trust.json"));
}

double TrustLedger::weightFor(const QString& pubkey) const
{
    auto it = m_direct.constFind(pubkey);
    if (it != m_direct.constEnd() && it->interactions > 0) {
        return it->score;
    }
    // Unseen author: bootstrap from transitive endorsements.
    const double bootstrap = bootstrapFloorFor(pubkey);
    return std::max(kFloor, std::min(kBootstrapCap, kFloor + bootstrap));
}

void TrustLedger::rewardAuthor(const QString& pubkey)
{
    Direct& d = m_direct[pubkey];
    d.score = clamp01_2(d.score + kAcceptDelta);
    d.interactions += 1;
    save();
    emit changed();
}

void TrustLedger::penalizeAuthor(const QString& pubkey)
{
    Direct& d = m_direct[pubkey];
    d.score = clamp01_2(d.score - kRejectDelta);
    d.interactions += 1;
    save();
    emit changed();
}

void TrustLedger::setEndorsementsFrom(const QString& author,
                                      const QHash<QString, double>& targets)
{
    if (author.isEmpty()) return;
    // Only act on endorsements from peers we already trust above the
    // publish cutoff (directly or transitively). Otherwise an attacker
    // could broadcast endorsements without first earning a foothold.
    const double w = weightFor(author);
    if (w < kPublishCutoff) {
        // Still store the row in case we trust the author later, but
        // bound total storage.
    }
    m_endorsements[author] = targets;

    // LRU-style cap: count total entries; if we exceed the cap, drop
    // the author with the smallest weight contribution.
    int total = 0;
    for (const auto& m : std::as_const(m_endorsements)) total += m.size();
    while (total > kMaxStoredEndorsements && !m_endorsements.isEmpty()) {
        // Drop the author whose own trust is lowest.
        QString worst;
        double worstW = std::numeric_limits<double>::infinity();
        for (auto it = m_endorsements.cbegin(); it != m_endorsements.cend(); ++it) {
            const double aw = weightFor(it.key());
            if (aw < worstW) { worstW = aw; worst = it.key(); }
        }
        if (worst.isEmpty()) break;
        total -= m_endorsements[worst].size();
        m_endorsements.remove(worst);
    }
    save();
    emit changed();
}

QHash<QString, double> TrustLedger::outboundEndorsements() const
{
    QList<QPair<QString, double>> sorted;
    sorted.reserve(m_direct.size());
    for (auto it = m_direct.cbegin(); it != m_direct.cend(); ++it) {
        if (it->score >= kPublishCutoff && it->interactions > 0) {
            sorted.append({it.key(), it->score});
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    QHash<QString, double> out;
    out.reserve(std::min(int(sorted.size()), kMaxOutbound));
    for (int i = 0; i < std::min(int(sorted.size()), kMaxOutbound); ++i) {
        out.insert(sorted[i].first, sorted[i].second);
    }
    return out;
}

double TrustLedger::bootstrapFloorFor(const QString& target) const
{
    // Sum over damped paths from "me" (= every directly-trusted author
    // above kPublishCutoff acts as a starting node, weighted by its own
    // direct trust) to `target`, up to kMaxDepth hops.
    //
    // Implemented as DFS with a visited set per path. The numerical
    // result is summed across all paths and capped at kBootstrapCap by
    // the caller (we return the raw sum so the caller does the cap).
    double total = 0.0;

    // Seed: every directly-trusted peer above publish cutoff.
    for (auto it = m_direct.cbegin(); it != m_direct.cend(); ++it) {
        if (it->interactions <= 0) continue;
        if (it->score < kPublishCutoff) continue;
        const QString& seed = it.key();
        // DFS from `seed`. Path-product starts at score(seed).
        struct Frame {
            QString node;
            double  product;
            int     depth;
            QSet<QString> visited;
        };
        QList<Frame> stack;
        stack.append({ seed, it->score, 1, { seed } });
        while (!stack.isEmpty()) {
            Frame f = stack.takeLast();
            // Does this node endorse the target?
            auto endIt = m_endorsements.constFind(f.node);
            if (endIt != m_endorsements.constEnd()) {
                auto edgeIt = endIt->constFind(target);
                if (edgeIt != endIt->constEnd()) {
                    const double damp  = std::pow(kHopDamping, double(f.depth));
                    const double edgeS = *edgeIt;
                    total += damp * f.product * edgeS;
                }
                if (f.depth < kMaxDepth) {
                    for (auto neigh = endIt->cbegin(); neigh != endIt->cend(); ++neigh) {
                        const QString& next = neigh.key();
                        if (f.visited.contains(next)) continue;
                        // Chain through endorsements where the next hop
                        // also has its own outbound endorsements.
                        if (!m_endorsements.contains(next)) continue;
                        Frame nf;
                        nf.node    = next;
                        nf.product = f.product * neigh.value();
                        nf.depth   = f.depth + 1;
                        nf.visited = f.visited;
                        nf.visited.insert(next);
                        stack.append(nf);
                    }
                }
            }
        }
    }
    return total;
}

void TrustLedger::reset()
{
    m_direct.clear();
    m_endorsements.clear();
    save();
    emit changed();
}

void TrustLedger::load()
{
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray buf = f.readAll();
    f.close();
    if (buf.isEmpty()) return;
    nlohmann::json j;
    try { j = nlohmann::json::parse(buf.constData(), buf.constData() + buf.size()); }
    catch (...) { return; }

    m_direct.clear();
    m_endorsements.clear();

    if (j.is_object()) {
        if (j.contains("direct") && j["direct"].is_object()) {
            for (auto it = j["direct"].begin(); it != j["direct"].end(); ++it) {
                Direct d;
                d.score        = it.value().value("score", double(kFloor));
                d.interactions = it.value().value("n", 0);
                m_direct.insert(QString::fromStdString(it.key()), d);
            }
        }
        if (j.contains("endorsements") && j["endorsements"].is_array()) {
            for (const auto& row : j["endorsements"]) {
                const QString author = QString::fromStdString(row.value("author", std::string()));
                const QString target = QString::fromStdString(row.value("target", std::string()));
                const double  score  = row.value("score", 0.0);
                if (author.isEmpty() || target.isEmpty()) continue;
                m_endorsements[author].insert(target, score);
            }
        }
    }
}

void TrustLedger::save() const
{
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).path());

    nlohmann::json j;
    j["direct"] = nlohmann::json::object();
    for (auto it = m_direct.cbegin(); it != m_direct.cend(); ++it) {
        nlohmann::json row;
        row["score"] = it->score;
        row["n"]     = it->interactions;
        j["direct"][it.key().toStdString()] = row;
    }
    j["endorsements"] = nlohmann::json::array();
    for (auto a = m_endorsements.cbegin(); a != m_endorsements.cend(); ++a) {
        for (auto t = a->cbegin(); t != a->cend(); ++t) {
            nlohmann::json row;
            row["author"] = a.key().toStdString();
            row["target"] = t.key().toStdString();
            row["score"]  = t.value();
            j["endorsements"].push_back(row);
        }
    }
    const std::string out = j.dump(2);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(out.data(), qsizetype(out.size()));
    f.commit();
}

} // namespace censorcut
