#include "ffmpegPlayer.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    ffmpegPlayer w;
	w.playStream("");
    w.show();
    return a.exec();
}
