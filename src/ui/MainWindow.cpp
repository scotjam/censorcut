#include "MainWindow.h"

#include "TimelineWidget.h"
#include "VideoSurface.h"
#include "core/MarkerModel.h"
#include "core/PlaybackController.h"
#include "core/Project.h"

#include <QAction>
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
    setWindowTitle(QStringLiteral("CensorCut"));
    resize(1200, 800);

    m_playback = std::make_unique<PlaybackController>(this);
    m_markers  = new MarkerModel(this);

    buildUi();
    buildMenus();
    connectSignals();
    updateStatusBar();
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
    m_playButton->setShortcut(Qt::Key_Space);
    tLayout->addWidget(m_playButton);

    m_timeLabel = new QLabel(QStringLiteral("00:00:00.000 / 00:00:00.000"), transport);
    tLayout->addWidget(m_timeLabel);
    tLayout->addStretch(1);

    auto* hint = new QLabel(
        QStringLiteral("[ start cut · ] end cut · Esc cancel pending · ←/→ seek 5s · ,/. frame · J/K/L shuttle"),
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

    auto* analyzerPlaceholder = new QLabel(
        QStringLiteral("Analyzer panel — wired up in M3."), splitter);
    analyzerPlaceholder->setAlignment(Qt::AlignCenter);
    analyzerPlaceholder->setStyleSheet(QStringLiteral("color:#888; padding:24px;"));

    splitter->addWidget(m_markerList);
    splitter->addWidget(analyzerPlaceholder);
    splitter->setStretchFactor(1, 1);
    splitter->setMinimumHeight(200);
    mainLayout->addWidget(splitter);

    setCentralWidget(central);
    statusBar();
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

    connect(m_markerList, &QListView::doubleClicked, this,
            [this](const QModelIndex& ix) {
                if (auto m = m_markers->markerAt(ix.row())) {
                    m_playback->seek(m->startMs);
                }
            });

    connect(m_markerList, &QWidget::customContextMenuRequested,
            this, &MainWindow::onMarkerListContextMenu);

    connect(m_markers, &MarkerModel::markerAdded,   this, [this]{ m_dirty = true; updateStatusBar(); });
    connect(m_markers, &MarkerModel::markerRemoved, this, [this]{ m_dirty = true; updateStatusBar(); });
    connect(m_markers, &MarkerModel::markerChanged, this, [this]{ m_dirty = true; updateStatusBar(); });
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
        case Qt::Key_Left:
            m_playback->seekRelative(-5000);
            return;
        case Qt::Key_Right:
            m_playback->seekRelative(+5000);
            return;
        case Qt::Key_Comma:
            m_playback->stepFrame(-1);
            return;
        case Qt::Key_Period:
            m_playback->stepFrame(+1);
            return;
        case Qt::Key_J:
            m_playback->setRate(std::max(0.25, m_playback->rate() / 2.0));
            return;
        case Qt::Key_K:
            m_playback->pause();
            return;
        case Qt::Key_L:
            m_playback->setRate(std::min(4.0, m_playback->rate() * 2.0));
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
    setWindowTitle(QStringLiteral("CensorCut — %1").arg(QFileInfo(path).fileName()));
    m_pendingCutStartMs = -1;
    m_timeline->setPendingCutStartMs(-1);
    loadProjectFor(path);
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
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(sidecar), 3000);
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
}

void MainWindow::onDurationKnown(qint64 ms)
{
    m_timeline->setDurationMs(ms);
    m_timeLabel->setText(QStringLiteral("%1 / %2")
                             .arg(formatTime(m_playback->position()), formatTime(ms)));
}

void MainWindow::onPlayingStateChanged(bool playing)
{
    m_playButton->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
}

void MainWindow::onTimelineScrubbed(qint64 ms)
{
    m_playback->seek(ms);
}

void MainWindow::onMarkerListContextMenu(const QPoint& pos)
{
    const QModelIndex ix = m_markerList->indexAt(pos);
    if (!ix.isValid()) return;
    auto m = m_markers->markerAt(ix.row());
    if (!m) return;

    QMenu menu(this);
    auto* play = menu.addAction(QStringLiteral("Play this marker"));
    auto* confirm = menu.addAction(QStringLiteral("Confirm"));
    auto* reject  = menu.addAction(QStringLiteral("Reject"));
    menu.addSeparator();
    auto* del = menu.addAction(QStringLiteral("Delete"));

    confirm->setEnabled(m->status != Status::Confirmed);
    reject->setEnabled(m->status != Status::Rejected);

    QAction* chosen = menu.exec(m_markerList->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == play) {
        m_playback->seek(m->startMs);
        m_playback->play();
    } else if (chosen == confirm) {
        m_markers->setData(ix, static_cast<int>(Status::Confirmed), MarkerModel::StatusRole);
    } else if (chosen == reject) {
        m_markers->setData(ix, static_cast<int>(Status::Rejected), MarkerModel::StatusRole);
    } else if (chosen == del) {
        m_markers->removeMarkerAt(ix.row());
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
