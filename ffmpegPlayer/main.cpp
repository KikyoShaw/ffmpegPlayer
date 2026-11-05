#include "ffmpegPlayer.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication a(argc, argv);
    ffmpegPlayer w;
    w.show();
    return a.exec();
}
