#include <QApplication>

#include "MainWindow.hpp"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // QSettings picks its own per-platform location from these, which is how
    // the converter remembers your paths and stage selection between runs
    // without any file handling of its own.
    QCoreApplication::setOrganizationName(QStringLiteral("RivenNDS"));
    QCoreApplication::setApplicationName(QStringLiteral("riven-convert"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1"));

    MainWindow window;
    window.show();
    return app.exec();
}
