// ---------------------------------------------------------------------------
//  main.cpp -- Servo Rig Android app entry point
//
//  Reconstructed file: CMakeLists.txt names "main.cpp" as the app's
//  executable source (qt_add_executable(appServoRig main.cpp)), but no
//  such file survived alongside servolink.cpp/mjpegclient.cpp/
//  videosurface.cpp/Main.qml. This is the standard Qt 6.5+ QML
//  application entry point for a qt_add_qml_module-based project: it
//  just starts the engine and loads Main.qml from the "ServoRig" module
//  registered in CMakeLists.txt.
// ---------------------------------------------------------------------------

#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    // If Main.qml fails to load (a typo in a binding, a missing type),
    // exit cleanly instead of leaving a blank window open.
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    // "ServoRig" / "Main" matches qt_add_qml_module(appServoRig URI ServoRig
    // ... QML_FILES Main.qml) in CMakeLists.txt.
    engine.loadFromModule("ServoRig", "Main");

    return app.exec();
}
