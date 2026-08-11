# Corne + trackball, version con dongle

Variante **experimental** de `5x4_track` en la que un tercer nRF52840 (clon de
nice!nano) enchufado por USB hace de central del split. Repo aparte a proposito:
el original sigue funcionando y no se toca.

## Que cambia respecto al original

|                | Antes                  | Ahora                     |
| -------------- | ---------------------- | ------------------------- |
| Central        | Mitad izquierda        | **Dongle** (USB)          |
| Izquierda      | Central                | Periferica                |
| Derecha        | Periferica + trackball | Periferica + trackball    |
| HID al PC      | BLE desde la izquierda | **USB desde el dongle**   |

El keymap es exactamente el mismo: `config/corne_dongle.keymap` no es una copia,
solo hace `#include "corne.keymap"`.

## Que se gana

- **Bateria en la izquierda.** Era la central: mantenia BLE con el PC y con la
  derecha, ademas de procesar el stream del trackball. Ahora las dos mitades son
  perifericas y solo hablan con el dongle.
- **Puntero mas estable.** El ultimo salto pasa de BLE a USB.
- Funciona en la BIOS, en maquinas sin Bluetooth y detras de un KVM.

## Orden de flasheo (importante)

Los bonds de la configuracion anterior impiden que las mitades se conecten al
dongle. Hay que limpiarlos **antes**:

1. `settings_reset` en las tres piezas (izquierda, derecha y dongle).
2. `corne_dongle` en el dongle.
3. `corne_left` en la izquierda y `corne_right` en la derecha.
4. Enchufar el dongle y esperar a que las mitades se emparejen solas.

Los cuatro `.uf2` salen del mismo run de GitHub Actions.

## Sin verificar todavia

Nada de esto se ha compilado ni probado aun. Los puntos con mas probabilidad de
dar guerra, por orden:

- **`&pmwcpi`.** Usa `locality = BEHAVIOR_LOCALITY_EVENT_SOURCE`, asi que se
  ejecuta en la mitad donde se pulsa la tecla. Sus teclas estan en el bloque
  derecho del keymap, que es donde vive el sensor, asi que en teoria sigue
  funcionando igual que antes. El dongle compilara ese behavior en su rama
  `-ENODEV` (no tiene nodo `trackball`), que es lo correcto.
- **Trackball reenviado + dongle a la vez.** ZMK documenta el dongle y documenta
  el `zmk,input-split` multi-periferica por separado, pero no la combinacion.
- **La matriz de `corne_dongle.overlay`** es copia literal del `default_transform`
  del shield corne oficial. Si cambia alli, hay que sincronizarla.
- **La nice!view de la izquierda** seguira encendiendo, pero como periferica
  muestra menos informacion que como central.
