#pragma once

#include "core/ExportPlan.h"
#include "core/Project.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QRadioButton;

namespace censorcut {

/// Configure a single export. The dialog itself does NOT run the export —
/// it just collects (source, output, quality) and a per-user auto-save
/// preference. The caller (MainWindow) reads outputPath()/quality()/
/// autoSaveOnStart() after a successful exec() and pushes a job onto the
/// ExportQueue.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(QWidget* parent = nullptr);

    void setProject(const Project& project);

    QString       outputPath()     const { return m_outputPath; }
    ExportQuality quality()        const;
    bool          autoSaveOnStart() const;

private slots:
    void onAddToQueue();

private:
    void rebuildOutputPath();

    Project       m_project;
    bool          m_haveProject = false;
    QString       m_outputPath;

    QLabel*       m_sourceLabel    = nullptr;
    QLabel*       m_outputLabel    = nullptr;
    QRadioButton* m_radioAccurate  = nullptr;
    QRadioButton* m_radioFast      = nullptr;
    QCheckBox*    m_autoSaveCheck  = nullptr;
};

} // namespace censorcut
