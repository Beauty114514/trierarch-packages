plugins {
    id("com.android.library") version "9.3.1"
}

android {
    namespace = "app.trierarch.x11host"
    compileSdk = 34
    ndkVersion = "29.0.14206865"

    defaultConfig {
        minSdk = 26

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                targets += "Xlorie"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/lorie/CMakeLists.txt")
        }
    }
}
