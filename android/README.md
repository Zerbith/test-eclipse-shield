# Eclipse für Android TV / NVIDIA Shield (Entwurf)

**Status: erster Entwurf, noch nie gebaut oder auf einer Shield gestartet.** Er entstand ohne
Android-NDK und ohne Gerät. Rechne damit, beim ersten CI-Lauf Fehler zu beheben.

## APK bauen (empfohlen: GitHub Actions)

1. Forke dieses Repository auf GitHub und pushe diese Änderungen in deinen Fork.
2. Öffne **Actions → Build Android (NVIDIA Shield / Android TV) → Run workflow**.
3. Nach dem Lauf liegt `eclipse-android-arm64.apk` unter **Artifacts**.
4. Schlägt ein Schritt fehl, steht im Log, welche Bibliothek betroffen ist
   (`android/scripts/build-native.sh` baut jede Abhängigkeit in einer eigenen Funktion).

## Auf die Shield installieren

```
adb connect <IP-der-Shield>      # Entwickleroptionen + USB-/Netzwerk-Debugging auf der Shield aktivieren
adb install -r eclipse-android-arm64.apk
```

Die APK ist mit dem Debug-Schlüssel signiert. Für eigene Verteilung einen eigenen Keystore nutzen.

## Was dieser Entwurf enthält

| Teil | Datei |
| --- | --- |
| Hardware-Decoder (H.264/HEVC über `AMediaCodec`) als SS4S-Modul | `android/ss4s-mediacodec/` |
| Android als CMake-Plattform, App als `libmain.so` | `CMakeLists.txt`, `src/main.c` |
| Schriften aus `/system/fonts` statt Fontconfig | `src/app/util/font_android.c` |
| UI ohne gettext (englisch) | `src/app/platform/common/i18n_android.c` |
| Transparente UI über dem Video | `src/app/ui/root.c`, `EclipseActivity.java` |
| Android-TV-Gerüst (Leanback-Manifest, Activity, Banner) | `android/app/` |
| Abhängigkeiten cross-kompilieren, Libraries einsammeln | `android/scripts/build-native.sh` |
| CI-Pipeline | `.github/workflows/build-android.yml` |

## Bekannte offene Punkte und Risiken

- **Nicht kompiliert:** Das Modul ist nur gegen Platzhalter-Header geprüft, nicht gegen das echte NDK.
- **Modul-Ladepfad:** ss4s lädt Module per `dlopen`. Das Build-Skript patcht `module_impl.h`, damit
  auf Android `libss4s-<name>.so` gesucht wird. Ob die Modulliste (`ss4s_modules.ini`) dabei
  gefunden wird, ist ungetestet.
- **Video-Fläche:** Das Video läuft auf einer eigenen `SurfaceView` hinter der SDL-Fläche. Falls das Bild
  schwarz bleibt, ist `getVideoSurface()` bzw. die Z-Reihenfolge der erste Verdächtige.
- **Fehlt noch:** HDR10, AV1, Surround-Ton, Gamepad-Feinheiten (Rumble/Gyro), Co-op auf Android,
  deutsche Übersetzung, eigenes App-Icon.
- **Abhängigkeitsversionen** (mbedTLS 2.28, curl 8.9, SDL 2.30 …) sind Annahmen; passen sie nicht zu
  `moonlight-common-c`, im Skript anpassen.
- **Lizenz:** Eclipse steht unter GPL-3.0. Weitergegebene APKs brauchen den passenden Quellcode.
