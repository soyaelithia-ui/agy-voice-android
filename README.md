# agy-voice-android

Puente nativo en C con **Android AAudio (`libaaudio.so`)** que implementa `arecord` y `aplay` de baja latencia en Termux para habilitar `/voice live` y `/voice dictate` directamente en **Antigravity CLI (`agy`)** en Android (sin PC y sin root).

## Requisitos en Termux

```bash
pkg install -y clang termux-api
```
*(Asegúrate de tener instalada la app **Termux:API** y conceder el permiso de micrófono cuando lo solicite `termux-microphone-record -i`).*

## Instalación rápida en cualquier dispositivo Termux

```bash
git clone https://github.com/soyaelithia-ui/agy-voice-android.git
cd agy-voice-android
./build.sh
termux-microphone-record -i
```

Luego abre `agy` y escribe:
```text
/voice live
```
Para salir del modo voz escribe `/voice stop`.
