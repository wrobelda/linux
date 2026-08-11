.. SPDX-License-Identifier: GPL-2.0-only

****************************************
Xiaomi Pad pogo keyboard power controller
****************************************

Purpose
-------

``xiaomi_keyboard`` is a platform driver for the keyboard MCU power and wake
wiring in the Xiaomi Pad 5 family.  The MCU is inside the tablet, while the
detachable folio contains the keyboard matrix and pogo contacts.  The driver
powers the MCU even when the folio is absent, so its USB HID bridge remains
enumerated and can detect a later attachment.

This driver does not handle keystrokes, pointer data, or folio attachment.
Those signals travel through USB HID.  ``hid-generic`` and
``hid-multitouch`` handle ordinary input, while ``hid-xiaomi-pogo`` handles
the vendor status protocol and reports ``SW_TABLET_MODE``.  See
:doc:`../../hid/xiaomi-pogo` for the USB layout and protocol.

Platform wiring
---------------

The driver matches ``xiaomi,pad-5-keyboard``.  The DT node provides:

* ``vdd-gpios``, which enables the MCU supply;
* ``reset-gpios``, which releases the MCU from reset;
* ``irq-gpios``, which receives a rising-edge wake doorbell from the MCU;
* ``default`` and ``sleep`` pinctrl states.

The enable signal belongs to the pinctrl states.  The active state pulls it
high, while the suspend state pulls it low.

The wake doorbell is an event signal, not a level-valued presence signal.  It
can wake the system when MCU activity occurs, but sampling its level cannot
tell whether the folio is attached.  Attachment state must instead come from
the Nanosic status message handled by ``hid-xiaomi-pogo``.

Power and suspend sequence
--------------------------

At probe, the driver performs this sequence:

1. assert VDD and wait 1 to 2 ms;
2. deassert reset and wait 2 to 3 ms;
3. select the ``default`` pinctrl state;
4. request the wake doorbell IRQ and enable device wake-up.

If pinctrl setup or IRQ registration fails, the driver returns the MCU to
reset and disables VDD.  Removing the driver selects ``sleep``,
asserts reset, and disables VDD.

System suspend selects ``sleep`` and enables the doorbell as a wake IRQ.
System resume disables IRQ wake and restores ``default``.  VDD and
reset remain asserted across system suspend, so the MCU can retain its state
and signal activity.

Difference from the downstream driver
-------------------------------------

Xiaomi's downstream implementation combines GPIO control with a larger
command-driven state machine and sysfs control surface.  Android userspace
drives that interface as display and power state changes.  The Linux driver
keeps only the hardware operations which belong to the platform device, so it
does not depend on an Android HAL or daemon and does not duplicate keyboard
attachment state.

The division is deliberate: platform power sequencing remains here, while
USB protocol handling remains in the HID subsystem.  This also lets the
keyboard and digitizer use their normal HID drivers without passing their
reports through a platform-specific input implementation.
