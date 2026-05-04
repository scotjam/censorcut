#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("CensorCut"));
    QApplication::setOrganizationName(QStringLiteral("CensorCut"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    censorcut::MainWindow w;
    w.show();
    return app.exec();
}
