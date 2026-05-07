#include "EditsPullDialog.h"

#include "core/Marker.h"
#include "core/MarkerModel.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace censorcut {

namespace {

QString labelForRow(const EditsPullDialog* /*unused*/, const EditPack& p,
                    const MatchVerdict& v)
{
    QString matchTag;
    if (v.isSameFilm) {
        matchTag = QStringLiteral("✓ %1/%2 anchors match")
                       .arg(v.matchedAnchors).arg(v.totalAnchors);
        if (v.alignment) {
            const auto& a = *v.alignment;
            matchTag += QStringLiteral("  (offset %1 ms, scale %2)")
                            .arg(a.offsetMs)
                            .arg(QString::number(a.scale, 'f', 3));
        }
    } else {
        matchTag = QStringLiteral("✗ %1/%2 anchors match")
                       .arg(v.matchedAnchors).arg(v.totalAnchors);
    }
    QString summary = QStringLiteral("%1 cuts  ·  %2  ·  by %3…")
                          .arg(p.cuts.size())
                          .arg(matchTag)
                          .arg(p.authorPubkey.left(12));
    if (!p.comment.isEmpty()) {
        summary += QStringLiteral("\n    %1").arg(p.comment.left(160));
    }
    return summary;
}

} // namespace

EditsPullDialog::EditsPullDialog(MarkerModel* markers,
                                 const FilmFingerprint& localFp,
                                 QWidget* parent)
    : QDialog(parent), m_markers(markers), m_localFp(localFp)
{
    setWindowTitle(tr("Pull edit packs from server"));
    setMinimumSize(720, 380);

    auto* main = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    main->addWidget(m_status);

    m_listWidget = new QListWidget(this);
    m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listWidget->setAlternatingRowColors(true);
    main->addWidget(m_listWidget, /*stretch=*/1);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    m_applyBtn = new QPushButton(tr("Apply selected"), this);
    m_applyBtn->setEnabled(false);
    m_closeBtn = new QPushButton(tr("Close"), this);
    btns->addWidget(m_applyBtn);
    btns->addWidget(m_closeBtn);
    main->addLayout(btns);

    m_client = new EditsClient(this);
    connect(m_client, &EditsClient::packsFetched,
            this, &EditsPullDialog::onPacksFetched);
    connect(m_client, &EditsClient::fetchFailed,
            this, &EditsPullDialog::onFetchFailed);

    connect(m_applyBtn, &QPushButton::clicked, this, &EditsPullDialog::onApplyClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_listWidget, &QListWidget::itemSelectionChanged,
            this, &EditsPullDialog::onSelectionChanged);
}

void EditsPullDialog::start(const QUrl& serverUrl)
{
    if (!m_localFp.isValid()) {
        m_status->setText(tr("This movie has no fingerprint yet — run analysis first."));
        return;
    }
    m_status->setText(tr("Fetching from %1…").arg(serverUrl.toString()));
    m_client->fetch(serverUrl, m_localFp.digest);
}

void EditsPullDialog::onPacksFetched(const QString& filmFp, const QList<EditPack>& packs)
{
    Q_UNUSED(filmFp);
    m_rows.clear();
    m_listWidget->clear();
    int sameFilm = 0;
    for (const EditPack& p : packs) {
        FilmFingerprint remote;
        remote.durationMs = 0;
        remote.digest     = p.filmFp;
        remote.anchors    = p.filmAnchors;
        const auto v = matchFingerprints(m_localFp, remote);
        if (v.isSameFilm) ++sameFilm;
        Row row{ p, v };
        const QString label = labelForRow(this, p, v);
        auto* item = new QListWidgetItem(label, m_listWidget);
        if (!v.isSameFilm) {
            item->setForeground(Qt::darkGray);
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        }
        m_rows.append(row);
    }
    m_status->setText(tr("Server returned %1 pack(s); %2 match this film.")
                          .arg(packs.size()).arg(sameFilm));
}

void EditsPullDialog::onFetchFailed(const QString& reason)
{
    m_status->setText(tr("Could not fetch: %1").arg(reason));
}

void EditsPullDialog::onSelectionChanged()
{
    const int row = m_listWidget->currentRow();
    if (row < 0 || row >= m_rows.size()) {
        m_applyBtn->setEnabled(false);
        return;
    }
    m_applyBtn->setEnabled(m_rows.at(row).verdict.isSameFilm);
}

void EditsPullDialog::onApplyClicked()
{
    const int idx = m_listWidget->currentRow();
    if (idx < 0 || idx >= m_rows.size()) return;
    const Row& row = m_rows.at(idx);
    if (!row.verdict.isSameFilm || !row.verdict.alignment) return;
    if (!m_markers) return;

    const auto& fit = *row.verdict.alignment;
    int added = 0;
    for (const EditPackCut& c : row.pack.cuts) {
        Marker m;
        m.startMs    = mapTime(fit, c.startMs);
        m.endMs      = mapTime(fit, c.endMs);
        if (m.endMs <= m.startMs) continue;
        m.category   = c.category;
        m.source     = Source::Suggested;
        m.confidence = (c.score >= 0.0 && c.score <= 1.0) ? c.score : 0.5;
        m.status     = Status::Pending;
        QStringList notes;
        notes << tr("from edits server pack");
        if (!c.reason.isEmpty()) notes << c.reason;
        if (fit.scale != 1.0 || fit.offsetMs != 0) {
            notes << tr("aligned (offset %1 ms, scale %2)")
                         .arg(fit.offsetMs)
                         .arg(QString::number(fit.scale, 'f', 3));
        }
        m.note = notes.join(QStringLiteral(" · "));
        if (!row.pack.authorPubkey.isEmpty())
            m.contributingAuthors.append(row.pack.authorPubkey);
        m_markers->addMarker(m);
        ++added;
    }
    Q_UNUSED(added);
    accept();
}

} // namespace censorcut
