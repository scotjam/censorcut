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
    const QString tick = v.isSameFilm ? QStringLiteral("✓")
                                          : QStringLiteral("✗");
    QString matchTag = QStringLiteral("%1 %2").arg(tick).arg(v.reason);
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
    m_client->fetch(serverUrl, m_localFp.bucketKey());
}

void EditsPullDialog::onPacksFetched(const QString& filmId, const QList<EditPack>& packs)
{
    Q_UNUSED(filmId);
    m_rows.clear();
    m_listWidget->clear();
    int sameFilm = 0;
    for (const EditPack& p : packs) {
        const auto v = matchFingerprints(m_localFp, p.fingerprint);
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
    if (!row.verdict.isSameFilm) return;
    if (!m_markers) return;

    // F and v9 both collapse alignment to a single offset: the pack's
    // cuts live in the remote timeline; we add estimatedTrimMs to land
    // in the local timeline.
    const qint64 trim = row.verdict.estimatedTrimMs;
    int added = 0;
    for (const EditPackCut& c : row.pack.cuts) {
        Marker m;
        m.startMs    = c.startMs + trim;
        m.endMs      = c.endMs   + trim;
        if (m.endMs <= m.startMs) continue;
        m.category   = c.category;
        m.source     = Source::Suggested;
        m.confidence = (c.score >= 0.0 && c.score <= 1.0) ? c.score : 0.5;
        m.status     = Status::Pending;
        QStringList notes;
        notes << tr("from edits server pack");
        if (!c.reason.isEmpty()) notes << c.reason;
        if (trim != 0) {
            notes << tr("aligned (trim %1 ms)").arg(trim);
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
