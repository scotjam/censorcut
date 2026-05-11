#pragma once

#include "core/AnalysisResult.h"
#include "core/EditsClient.h"
#include "core/FingerprintMatcher.h"

#include <QDialog>
#include <QString>
#include <QUrl>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace censorcut {

class MarkerModel;

/// Modal dialog: fetches edit packs from a configured server for a
/// given film fingerprint, runs each pack through the M8.3 matcher,
/// and lets the user pick one to apply. On apply, the pack's cuts are
/// time-mapped through the affine alignment and pushed into the
/// MarkerModel as Source::Suggested markers (so they flow through the
/// existing review/confirm UI).
class EditsPullDialog : public QDialog {
    Q_OBJECT
public:
    EditsPullDialog(MarkerModel* markers,
                    const FilmFingerprint& localFingerprint,
                    QWidget* parent = nullptr);

    /// Begin the fetch. Caller passes the server URL (typically read
    /// from QSettings via EditsClient::configuredServerUrl).
    void start(const QUrl& serverUrl);

private slots:
    void onPacksFetched(const QString& filmId, const QList<EditPack>& packs);
    void onFetchFailed(const QString& reason);
    void onApplyClicked();
    void onSelectionChanged();

private:
    struct Row {
        EditPack            pack;
        MatchVerdict        verdict;
    };

    MarkerModel*       m_markers       = nullptr;
    FilmFingerprint    m_localFp;
    EditsClient*       m_client        = nullptr;

    QLabel*            m_status        = nullptr;
    QListWidget*       m_listWidget    = nullptr;
    QPushButton*       m_applyBtn      = nullptr;
    QPushButton*       m_closeBtn      = nullptr;

    QList<Row>         m_rows;
};

} // namespace censorcut
