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

## Estado

**Funciona.** Flasheado y probado: las dos mitades se emparejan con el dongle y
el teclado escribe correctamente.

Pendiente de confirmar: el trackball (movimiento, scroll en la capa 5 y el
cambio de CPI con `&pmwcpi`) y que `&rgb_ug` llegue a las dos mitades desde el
dongle.

### Dos fallos que costaron encontrar

1. **`undefined node label 'led_strip'`** al compilar. `led_strip` no lo define
   la placa nice!nano sino cada shield, en `boards/<placa>.overlay`. El shield
   del dongle no lo tenia y `corne.keymap:16` hace `&led_strip { ... }`, que
   compilan las tres piezas. Ver
   `boards/shields/corne_dongle/boards/nice_nano_nrf52840_zmk.overlay`.

2. **Teclas corridas un lugar** (la Q daba ESC, la Y daba T). El shield del
   dongle declaraba solo `foostan_corne_6col_layout`, y las mitades declaran los
   dos layouts, 5col y 6col. Los physical layouts de la central tienen que ser
   identicos a los de las demas piezas.

Los puntos con mas probabilidad de dar guerra todavia, por orden:

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
