#include "ffmpegPlayer.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    ffmpegPlayer w;
	w.playStream("http://vfx.mtime.cn/Video/2019/03/14/mp4/190314223540373995.mp4");
    w.show();
    return a.exec();
}
