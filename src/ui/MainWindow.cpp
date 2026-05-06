#include "MainWindow.h"

#include "AnalyzerPanel.h"
#include "ExportDialog.h"
#include "ExportQueuePanel.h"
#include "TimelineWidget.h"
#include "VideoSurface.h"
#include "core/ExportQueue.h"
#include "core/FeedbackStore.h"
#include "core/MarkerModel.h"
#include "core/PlaybackController.h"
#include "core/Project.h"

#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QPushButton>
#include <QSettings>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace censorcut {

namespace {

QString formatTime(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 sec = ms / 1000;
    const int h = static_cast<int>(sec / 3600);
    const int m = static_cast<int>((sec % 3600) / 60);
    const int s = static_cast<int>(sec % 60);
    const int ms_part = static_cast<int>(ms % 1000);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms_part, 3, 10, QChar('0'));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("CensorCut[*]"));
    resize(1200, 800);

    m_playback     = std::make_unique<PlaybackController>(this);
    m_markers      = new MarkerModel(this);
    m_exportQueue  = new ExportQueue(this);
    m_feedback     = new FeedbackStore(this);

    buildUi();
    buildMenus();
    connectSignals();
    updateStatusBar();

    // Show the disclaimer once on first launch (or when its text version is
    // bumped). Deferred to a single-shot so the main window has actually
    // started its event loop — modal dialogs need that to behave correctly.
    QMetaObject::invokeMethod(this, &MainWindow::maybeShowDisclaimer,
                              Qt::QueuedConnection);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_video = new VideoSurface(central);
    mainLayout->addWidget(m_video, /*stretch=*/1);

    // Transport bar
    auto* transport = new QWidget(central);
    auto* tLayout = new QHBoxLayout(transport);
    tLayout->setContentsMargins(8, 4, 8, 4);
    m_playButton = new QPushButton(QStringLiteral("Play"), transport);
    // Note: spacebar is handled in keyPressEvent so it works regardless of which
    // child widget has focus (the marker list, the timeline, etc).
    m_playButton->setFocusPolicy(Qt::NoFocus);
    tLayout->addWidget(m_playButton);

    m_timeLabel = new QLabel(QStringLiteral("00:00:00.000 / 00:00:00.000"), transport);
    tLayout->addWidget(m_timeLabel);

    m_previewCheck = new QCheckBox(QStringLiteral("Preview cuts"), transport);
    m_previewCheck->setToolTip(QStringLiteral(
        "When on, playback skips past confirmed cut markers so you can preview the result."));
    m_previewCheck->setFocusPolicy(Qt::NoFocus);
    tLayout->addWidget(m_previewCheck);

    tLayout->addStretch(1);

    auto* hint = new QLabel(
        QStringLiteral("Space play · [ ] mark cut · Esc cancel · ←/→ 5s · Shift+←/→ 1s · Ctrl+←/→ frame · J/K/L shuttle (J reverse) · 1 reset rate"),
        transport);
    hint->setStyleSheet(QStringLiteral("color:#888;"));
    tLayout->addWidget(hint);

    mainLayout->addWidget(transport);

    m_timeline = new TimelineWidget(central);
    m_timeline->setModel(m_markers);
    mainLayout->addWidget(m_timeline);

    // Splitter so the marker list can be resized; lives below the timeline.
    auto* splitter = new QSplitter(Qt::Horizontal, central);
    m_markerList = new QListView(splitter);
    m_markerList->setModel(m_markers);
    m_markerList->setMinimumWidth(280);
    m_markerList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_markerList->setSelectionMode(QAbstractItemView::ExtendedSelection);

    m_analyzer = new AnalyzerPanel(m_markers, m_playback.get(), m_feedback, splitter);

    splitter->addWidget(m_markerList);
    splitter->addWidget(m_analyzer);
    splitter->setStretchFactor(1, 1);
    splitter->setMinimumHeight(200);
    mainLayout->addWidget(splitter);

    setCentralWidget(central);
    statusBar();

    // Bottom dock: export queue panel.
    m_exportPanel = new ExportQueuePanel(m_exportQueue, this);
    auto* dock = new QDockWidget(QStringLiteral("Exports"), this);
    dock->setObjectName(QStringLiteral("ExportsDock"));
    dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    dock->setWidget(m_exportPanel);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
}

void MainWindow::buildMenus()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    auto* openAction = fileMenu->addAction(QStringLiteral("&Open Movie..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenFile);

    auto* saveAction = fileMenu->addAction(QStringLiteral("&Save Markers"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::onSaveSidecar);

    auto* exportAction = fileMenu->addAction(QStringLiteral("&Export Censored Cut..."));
    exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(exportAction, &QAction::triggered, this, &MainWindow::onExportProject);

    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    auto* markStart = editMenu->addAction(QStringLiteral("Mark cut &start"));
    markStart->setShortcut(QKeySequence(Qt::Key_BracketLeft));
    connect(markStart, &QAction::triggered, this, &MainWindow::onMarkStart);
    auto* markEnd = editMenu->addAction(QStringLiteral("Mark cut &end"));
    markEnd->setShortcut(QKeySequence(Qt::Key_BracketRight));
    connect(markEnd, &QAction::triggered, this, &MainWindow::onMarkEnd);
}

void MainWindow::connectSignals()
{
    // Once the video widget has a native window, attach VLC to it.
    // Use a queued call so winId() is valid.
    QMetaObject::invokeMethod(this, [this]{
        m_playback->setVideoSink(m_video->winId());
    }, Qt::QueuedConnection);

    connect(m_playButton, &QPushButton::clicked,
            m_playback.get(), &PlaybackController::togglePlayPause);
    connect(m_playback.get(), &PlaybackController::positionChanged,
            this, &MainWindow::onPositionChanged);
    connect(m_playback.get(), &PlaybackController::durationKnown,
            this, &MainWindow::onDurationKnown);
    connect(m_playback.get(), &PlaybackController::playingStateChanged,
            this, &MainWindow::onPlayingStateChanged);
    connect(m_playback.get(), &PlaybackController::errorOccurred, this,
            [this](const QString& msg) {
                QMessageBox::warning(this, QStringLiteral("Playback error"), msg);
            });

    connect(m_timeline, &TimelineWidget::scrubbed,
            m_playback.get(), &PlaybackController::seek);
    connect(m_timeline, &TimelineWidget::scrubBegan,
            this, [this]{ m_userScrubbing = true; });
    connect(m_timeline, &TimelineWidget::scrubEnded,
            this, [this]{ m_userScrubbing = false; });

    connect(m_markerList, &QListView::doubleClicked, this,
            [this](const QModelIndex& ix) {
                if (auto m = m_markers->markerAt(ix.row())) {
                    m_playback->seek(m->startMs);
                }
            });

    connect(m_markerList, &QWidget::customContextMenuRequested,
            this, &MainWindow::onMarkerListContextMenu);

    auto markDirty = [this]{
        m_dirty = true;
        setWindowModified(true);
        updateStatusBar();
    };
    connect(m_markers, &MarkerModel::markerAdded,   this, markDirty);
    connect(m_markers, &MarkerModel::markerRemoved, this, markDirty);
    connect(m_markers, &MarkerModel::markerChanged, this, markDirty);

    connect(m_previewCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_previewMode = on;
    });
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    const auto mods = event->modifiers();
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool ctrl  = mods.testFlag(Qt::ControlModifier);

    // NLE-style shuttle ladder. J steps left (toward reverse), L steps
    // right (toward fast-forward). Note: libVLC's reverse playback works
    // on most h264/hevc/etc. but a few exotic codecs may silently no-op.
    static constexpr double kRateLadder[] = {
        -4.0, -2.0, -1.0, -0.5, 0.5, 1.0, 2.0, 4.0,
    };
    static constexpr int kLadderLen = sizeof(kRateLadder) / sizeof(kRateLadder[0]);
    auto closestIndex = [](double r) {
        int idx = 5;  // 1.0 by default
        double best = 1e9;
        for (int i = 0; i < kLadderLen; ++i) {
            const double d = std::abs(kRateLadder[i] - r);
            if (d < best) { best = d; idx = i; }
        }
        return idx;
    };
    auto stepRate = [&](int delta) {
        const int idx = std::clamp(closestIndex(m_intendedRate) + delta, 0, kLadderLen - 1);
        m_intendedRate = kRateLadder[idx];
        m_playback->setRate(m_intendedRate);
        if (!m_playback->isPlaying()) m_playback->play();
        statusBar()->showMessage(
            QStringLiteral("Rate %1×").arg(m_intendedRate, 0, 'g', 2), 1500);
    };

    switch (event->key()) {
        case Qt::Key_Space:
            m_playback->togglePlayPause();
            return;
        case Qt::Key_Left:
            if (ctrl)        m_playback->stepFrame(-1);
            else if (shift)  m_playback->seekRelative(-1000);
            else             m_playback->seekRelative(-5000);
            return;
        case Qt::Key_Right:
            if (ctrl)        m_playback->stepFrame(+1);
            else if (shift)  m_playback->seekRelative(+1000);
            else             m_playback->seekRelative(+5000);
            return;
        case Qt::Key_Comma:
            m_playback->stepFrame(-1);
            return;
        case Qt::Key_Period:
            m_playback->stepFrame(+1);
            return;
        case Qt::Key_J:
            stepRate(-1);
            return;
        case Qt::Key_K:
            m_playback->pause();
            return;
        case Qt::Key_L:
            stepRate(+1);
            return;
        case Qt::Key_1:
            m_intendedRate = 1.0;
            m_playback->setRate(1.0);
            if (!m_playback->isPlaying()) m_playback->play();
            statusBar()->showMessage(QStringLiteral("Rate 1× (reset)"), 1500);
            return;
        case Qt::Key_Escape:
            if (m_pendingCutStartMs >= 0) {
                m_pendingCutStartMs = -1;
                m_timeline->setPendingCutStartMs(-1);
                statusBar()->showMessage(QStringLiteral("Pending cut start cancelled."), 2000);
                return;
            }
            break;
        default:
            break;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (hasUnsavedChanges()) {
        const auto reply = QMessageBox::question(
            this, QStringLiteral("Unsaved markers"),
            QStringLiteral("The marker list differs from the sidecar on disk.\n"
                           "Save markers before closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (reply == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (reply == QMessageBox::Save) onSaveSidecar();
    }
    event->accept();
}

bool MainWindow::hasUnsavedChanges() const
{
    if (m_currentMoviePath.isEmpty()) return false;

    const QString sidecar = Project::sidecarPathFor(m_currentMoviePath);
    const QString liveFp  = Project::markersFingerprint(m_markers->markers());

    QFileInfo info(sidecar);
    if (!info.exists()) {
        // No file yet — empty interface vs no file is "in sync"; any markers
        // mean unsaved work.
        return !m_markers->markers().isEmpty();
    }

    auto loaded = Project::loadFromSidecar(sidecar);
    if (!loaded) {
        // Couldn't parse what's on disk — be conservative and prompt.
        return true;
    }
    return liveFp != Project::markersFingerprint(loaded->markers);
}

void MainWindow::onOpenFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Movie"), {},
        QStringLiteral("Video files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v);;All files (*.*)"));
    if (path.isEmpty()) return;

    if (!m_playback->open(path)) {
        QMessageBox::warning(this, QStringLiteral("Open failed"),
                             QStringLiteral("Could not open: %1").arg(path));
        return;
    }
    m_currentMoviePath = path;
    setWindowTitle(QStringLiteral("CensorCut — %1[*]").arg(QFileInfo(path).fileName()));
    m_pendingCutStartMs = -1;
    m_timeline->setPendingCutStartMs(-1);
    loadProjectFor(path);
    m_analyzer->setMovie(path, m_playback->duration());
    m_playback->play();
}

void MainWindow::loadProjectFor(const QString& moviePath)
{
    const QString sidecar = Project::sidecarPathFor(moviePath);
    QFileInfo sidecarInfo(sidecar);
    if (!sidecarInfo.exists()) {
        m_markers->clear();
        m_dirty = false;
        updateStatusBar();
        return;
    }
    QString err;
    auto project = Project::loadFromSidecar(sidecar, &err);
    if (!project) {
        QMessageBox::warning(this, QStringLiteral("Sidecar load failed"), err);
        return;
    }
    // Optional: warn if hash mismatches (file changed since last edit).
    const QString currentHash = Project::computeSourceHash(moviePath);
    if (!project->sourceHash.isEmpty() && !currentHash.isEmpty()
        && project->sourceHash != currentHash) {
        QMessageBox::information(this, QStringLiteral("File changed"),
            QStringLiteral("The movie file has changed since these markers were saved.\n"
                           "Marker times may no longer line up correctly."));
    }
    m_markers->setMarkers(project->markers);
    m_dirty = false;
    setWindowModified(false);
    updateStatusBar();
}

void MainWindow::onSaveSidecar()
{
    if (m_currentMoviePath.isEmpty()) return;

    Project p;
    p.sourceFile  = m_currentMoviePath;
    p.sourceHash  = Project::computeSourceHash(m_currentMoviePath);
    p.durationMs  = m_playback->duration();
    p.markers     = m_markers->markers();
    // ExportSettings and AgeProfile use defaults in M1.

    const QString sidecar = Project::sidecarPathFor(m_currentMoviePath);
    QString err;
    if (!p.saveToSidecar(sidecar, &err)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
        return;
    }
    m_dirty = false;
    setWindowModified(false);
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(sidecar), 3000);
}

void MainWindow::onExportProject()
{
    if (m_currentMoviePath.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to export"),
                                 tr("Open a movie first."));
        return;
    }

    Project p;
    p.sourceFile    = m_currentMoviePath;
    p.sourceHash    = Project::computeSourceHash(m_currentMoviePath);
    p.durationMs    = m_playback->duration();
    p.markers       = m_markers->markers();
    p.activeProfile = AgeProfile::forAge(8);  // M1: no UI for age yet — use default

    if (p.durationMs <= 0) {
        QMessageBox::warning(this, tr("Cannot export"),
                             tr("The video duration isn't known yet — let it load fully and try again."));
        return;
    }

    // Auto-pause playback so audio doesn't keep playing during configuration.
    if (m_playback->isPlaying()) m_playback->pause();

    ExportDialog dlg(this);
    dlg.setProject(p);
    if (dlg.exec() != QDialog::Accepted) return;

    // Handle unsaved markers BEFORE the job is queued, so the sidecar that
    // future loads see matches what the export was generated from.
    if (hasUnsavedChanges()) {
        if (dlg.autoSaveOnStart()) {
            onSaveSidecar();
        } else {
            const auto reply = QMessageBox::question(
                this, tr("Save markers before export?"),
                tr("You have unsaved marker changes.\n\n"
                   "Save them to the sidecar before queueing the export?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            if (reply == QMessageBox::Cancel) return;
            if (reply == QMessageBox::Save)   onSaveSidecar();
        }
        // Re-snapshot the (now-saved) markers into the project we're about
        // to enqueue, so subsequent edits don't affect the queued job.
        p.markers = m_markers->markers();
    }

    m_exportQueue->enqueue(p, dlg.outputPath(), dlg.quality());
}

void MainWindow::onMarkStart()
{
    if (m_playback->duration() <= 0) return;
    m_pendingCutStartMs = m_playback->position();
    m_timeline->setPendingCutStartMs(m_pendingCutStartMs);
    statusBar()->showMessage(QStringLiteral("Cut start at %1").arg(formatTime(m_pendingCutStartMs)),
                             2000);
}

void MainWindow::onMarkEnd()
{
    if (m_pendingCutStartMs < 0) {
        statusBar()->showMessage(QStringLiteral("Press [ to set the cut start first."), 3000);
        return;
    }
    const qint64 endMs = m_playback->position();
    if (endMs <= m_pendingCutStartMs) {
        statusBar()->showMessage(QStringLiteral("Cut end must be after the cut start."), 3000);
        return;
    }
    Marker m;
    m.startMs = m_pendingCutStartMs;
    m.endMs   = endMs;
    m.category = QStringLiteral("Manual");
    m.source   = Source::Manual;
    m.status   = Status::Confirmed;
    m_markers->addMarker(m);

    m_pendingCutStartMs = -1;
    m_timeline->setPendingCutStartMs(-1);
}

void MainWindow::onPositionChanged(qint64 ms)
{
    m_timeline->setPositionMs(ms);
    m_timeLabel->setText(QStringLiteral("%1 / %2")
                             .arg(formatTime(ms), formatTime(m_playback->duration())));

    // Preview mode: when normal forward playback CROSSES INTO a confirmed
    // cut, jump past it so the user hears/sees the result of the export.
    //
    // Three guards keep this from fighting the user when they're navigating
    // by hand:
    //   1. Suppress while m_userScrubbing — dragging the scrubber overrides
    //      preview, otherwise the playhead bounces back as you drag.
    //   2. Only on FORWARD motion (ms > prev) — backward seeks (right-click
    //      "Play this marker", marker-list double-click on an earlier
    //      marker) override preview.
    //   3. Only on FRESH crossings (prev was outside the cut) — once you're
    //      already inside, we let you keep playing. Combined with (2), this
    //      means landing inside a cut via manual seek is fine.
    const qint64 prev = m_lastPlaybackPos;
    m_lastPlaybackPos = ms;
    if (!m_previewMode) return;
    if (m_userScrubbing) return;
    if (prev < 0 || ms <= prev) return;
    qint64 jumpTo = -1;
    for (const auto& m : m_markers->markers()) {
        if (m.status != Status::Confirmed || !m.isValid()) continue;
        if (ms >= m.startMs && ms < m.endMs && prev < m.startMs) {
            jumpTo = std::max(jumpTo, m.endMs);
        }
    }
    if (jumpTo >= 0) m_playback->seek(jumpTo);
}

void MainWindow::onDurationKnown(qint64 ms)
{
    m_timeline->setDurationMs(ms);
    m_timeLabel->setText(QStringLiteral("%1 / %2")
                             .arg(formatTime(m_playback->position()), formatTime(ms)));
    if (m_analyzer && !m_currentMoviePath.isEmpty())
        m_analyzer->setMovie(m_currentMoviePath, ms);
}

void MainWindow::onPlayingStateChanged(bool playing)
{
    m_playButton->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
}

void MainWindow::onTimelineScrubbed(qint64 ms)
{
    m_playback->seek(ms);
}

void MainWindow::maybeShowDisclaimer()
{
    static const QString kCurrentVersion = QStringLiteral("1");
    QSettings settings;
    if (settings.value(QStringLiteral("disclaimer/acceptedVersion")).toString()
        == kCurrentVersion) {
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Disclaimer — please read"));
    box.setIcon(QMessageBox::Information);
    box.setTextFormat(Qt::RichText);
    box.setText(tr(
        "<b>CensorCut is not a substitute for parental review.</b><br><br>"
        "It is not designed to take care of legal requirements as to what "
        "children can be shown. It should only be used to <i>further</i> "
        "censor videos that are already legally able to be shown to "
        "children at the appropriate age.<br><br>"
        "Automated detection (M3 onwards) may miss things, make mistakes, "
        "or hallucinate content that isn't there. It may also cut more "
        "than is necessary. <b>Review every suggestion before exporting.</b>"
        "<br><br>"
        "The software is provided <b>as is</b>, with no warranty. "
        "No responsibility can be taken for any data loss."));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Close);
    box.button(QMessageBox::Ok)->setText(tr("I understand"));
    box.button(QMessageBox::Close)->setText(tr("Quit"));
    box.setDefaultButton(QMessageBox::Ok);

    if (box.exec() == QMessageBox::Ok) {
        settings.setValue(QStringLiteral("disclaimer/acceptedVersion"),
                          kCurrentVersion);
    } else {
        // User declined — close the window. Closing the only window quits
        // the app once the event loop returns to its natural exit.
        close();
    }
}

void MainWindow::onMarkerListContextMenu(const QPoint& pos)
{
    QModelIndexList selected = m_markerList->selectionModel()->selectedRows();
    // If the user right-clicked a row that wasn't part of the selection,
    // treat that single row as the operation target — matches what most
    // users expect from list-view context menus.
    const QModelIndex underCursor = m_markerList->indexAt(pos);
    if (underCursor.isValid()
        && std::none_of(selected.cbegin(), selected.cend(),
                        [&](const QModelIndex& ix){ return ix.row() == underCursor.row(); })) {
        selected = {underCursor};
        m_markerList->setCurrentIndex(underCursor);
    }
    if (selected.isEmpty()) return;

    // Snapshot ids up-front so deletions don't invalidate the work list.
    QList<QUuid> ids;
    ids.reserve(selected.size());
    for (const QModelIndex& ix : selected) {
        if (auto m = m_markers->markerAt(ix.row())) ids.append(m->id);
    }
    if (ids.isEmpty()) return;

    QMenu menu(this);
    if (ids.size() == 1) {
        auto m = m_markers->findById(ids.first());
        if (!m) return;

        auto* play    = menu.addAction(tr("Play this marker"));
        auto* confirm = menu.addAction(tr("Confirm"));
        auto* reject  = menu.addAction(tr("Reject"));
        menu.addSeparator();
        auto* del     = menu.addAction(tr("Delete"));

        confirm->setEnabled(m->status != Status::Confirmed);
        reject->setEnabled(m->status != Status::Rejected);

        QAction* chosen = menu.exec(m_markerList->viewport()->mapToGlobal(pos));
        if (!chosen) return;
        if (chosen == play) {
            m_playback->seek(m->startMs);
            m_playback->play();
        } else if (chosen == confirm) {
            Marker copy = *m; copy.status = Status::Confirmed;
            m_markers->updateMarkerById(ids.first(), copy);
            if (m_feedback && m->source == Source::Suggested)
                m_feedback->recordDecision(copy, FeedbackStore::Decision::Accepted);
        } else if (chosen == reject) {
            Marker copy = *m; copy.status = Status::Rejected;
            m_markers->updateMarkerById(ids.first(), copy);
            if (m_feedback && m->source == Source::Suggested)
                m_feedback->recordDecision(copy, FeedbackStore::Decision::Rejected);
        } else if (chosen == del) {
            m_markers->removeMarkerById(ids.first());
        }
        return;
    }

    // Multi-selection: bulk operations.
    auto* confirmAll = menu.addAction(tr("Confirm %n marker(s)", nullptr, ids.size()));
    auto* rejectAll  = menu.addAction(tr("Reject %n marker(s)",  nullptr, ids.size()));
    menu.addSeparator();
    auto* deleteAll  = menu.addAction(tr("Delete %n marker(s)",  nullptr, ids.size()));

    QAction* chosen = menu.exec(m_markerList->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == confirmAll || chosen == rejectAll) {
        const Status target = (chosen == confirmAll) ? Status::Confirmed : Status::Rejected;
        const auto decision = (target == Status::Confirmed)
            ? FeedbackStore::Decision::Accepted
            : FeedbackStore::Decision::Rejected;
        for (const QUuid& id : ids) {
            auto m = m_markers->findById(id);
            if (!m) continue;
            if (m->status == target) continue;
            Marker copy = *m;
            copy.status = target;
            m_markers->updateMarkerById(id, copy);
            if (m_feedback && m->source == Source::Suggested)
                m_feedback->recordDecision(copy, decision);
        }
    } else if (chosen == deleteAll) {
        for (const QUuid& id : ids) m_markers->removeMarkerById(id);
    }
}

void MainWindow::updateStatusBar()
{
    const int n = m_markers->rowCount();
    const qint64 cutMs = m_markers->totalConfirmedCutMs();
    const qint64 dur   = m_playback ? m_playback->duration() : 0;
    QString msg;
    if (dur > 0) {
        const double pct = 100.0 * double(cutMs) / double(dur);
        msg = QStringLiteral("%1 markers · %2 confirmed cut (%3% of runtime)%4")
                  .arg(n).arg(formatTime(cutMs))
                  .arg(pct, 0, 'f', 1)
                  .arg(m_dirty ? QStringLiteral(" · unsaved") : QString{});
    } else {
        msg = QStringLiteral("%1 markers%2").arg(n)
                  .arg(m_dirty ? QStringLiteral(" · unsaved") : QString{});
    }
    statusBar()->showMessage(msg);
}

} // namespace censorcut
