.. SPDX-License-Identifier: GPL-2.0-only

********************************
Xiaomi Pad pogo keyboard bridge
********************************

Hardware layout
---------------

The Xiaomi Pad 5 family contains a Nanosic keyboard MCU in the tablet.  The
MCU is powered and reset by the ``xiaomi_keyboard`` platform driver, while a
separate GPIO acts as a wake-up doorbell.  That GPIO does not carry the
keyboard presence state.

The MCU exposes a USB HID device with vendor ID ``0x3206`` and product ID
``0x3ffc``.  This USB device remains enumerated when the folio keyboard is
removed, because the MCU and USB connection are part of the tablet.  USB
enumeration therefore cannot indicate whether the keyboard is attached.

The USB device has four HID interfaces:

* interface 0 carries the keyboard and a relative pointer collection;
* interface 1 carries a multitouch digitizer collection;
* interface 2 is the vendor control channel;
* interface 3 is the vendor event channel.

``hid-xiaomi-pogo`` binds only interfaces 2 and 3.  The ordinary HID
interfaces remain available to ``hid-generic`` and ``hid-multitouch``, so the
driver does not reinterpret or filter keyboard, pointer, or digitizer input.

The same USB VID/PID is expected on the Xiaomi Pad 5 (nabu), Xiaomi Pad 5 Pro
(elish), Xiaomi Pad 5 Pro 5G (enuma), and Xiaomi Pad 5 Pro 12.4 (dagu)
keyboards.  These models can therefore use the same HID driver, although each
tablet still needs the platform support which powers its MCU.

Why attachment belongs in this driver
-------------------------------------

The detachable keyboard changes the way a desktop handles the device, so the
kernel reports its state as ``SW_TABLET_MODE``.  A set switch means that no
keyboard is attached, while a clear switch means that the keyboard is
attached.

Neither USB enumeration nor the MCU wake GPIO describes that state.  The MCU
does describe it in its vendor protocol, so the HID driver is the component
which has both reliable state information and the standard Linux input
interface needed by userspace.

The event channel sends changes asynchronously.  The driver also sends a
status query through the control channel during probe, because an event may
have occurred before the driver opened the interface.  This gives userspace a
correct initial state without requiring an attach and detach cycle.

Relationship to Xiaomi downstream drivers
-----------------------------------------

Xiaomi's downstream Pad 5 support uses a platform driver and a command-driven
state machine to sequence the MCU power, reset, suspend, and wake signals.  It
exports control through sysfs, while Android userspace coordinates those
transitions with display and power state.  This design is inefficient for a
mainline system because it repeats state in the kernel and userspace, performs
fixed-delay transitions, and depends on Android-specific userspace tooling.
It also does not turn the MCU's in-band keyboard status into a standard Linux
input switch.

The mainline drivers split these responsibilities at their bus boundary:

* ``xiaomi_keyboard`` controls the platform GPIOs and MCU power sequence;
* ``hid-xiaomi-pogo`` reads attachment state from the USB HID vendor channels;
* the generic HID drivers handle the keyboard and digitizer reports.

This split keeps the HID driver transparent to unrelated input while avoiding
the downstream dependency on a userspace-managed state machine.

Protocol origin
---------------

The protocol fields were identified from Xiaomi's downstream Nanosic driver
for pipa, the newer Xiaomi Pad 6.  Pipa uses the same Nanosic MCU class and
message format, although its hardware connects the MCU directly over I2C
instead of exposing these messages as USB HID reports.

The Pad 5 protocol was also verified independently.  HID traffic was traced
over repeated attach and detach cycles, then the status command was sent to a
live enuma tablet and its reply was checked.  This confirmed that the USB
transport removes the two-byte I2C framing prefix but leaves the Nanosic
message and checksum unchanged.

Message format
--------------

All multi-byte examples below are byte streams.  Checksums are the 8-bit sum
of every preceding byte in the Nanosic message.

The 32-byte HID output report used to query current state starts with::

    4e 31 80 38 a1 01 01 da

The remaining bytes are zero.  The fields have these meanings:

* ``4e`` is the output report ID;
* ``31`` is the Nanosic framing marker;
* ``80`` is the host address and ``38`` is the keyboard-controller address;
* ``a1`` requests status;
* ``01 01`` selects keyboard attachment status;
* ``da`` is the checksum.

On pipa's I2C transport, the same request has the prefix ``32 00`` and the
checksum follows the Nanosic message.  The complete request there is::

    32 00 4e 31 80 38 a1 01 01 da

The MCU returns the current state on HID input report ``0x24``.  Later state
changes arrive on input report ``0x26``.  Both reports are 64 bytes and share
the following checked fields:

* offset 0 is ``0x24`` for a current-state reply or ``0x26`` for an
  asynchronous-state report;
* offset 2 contains the keyboard-controller address ``0x38``;
* offset 3 contains the host address ``0x80``;
* offset 4 contains the status response ``0xa2``;
* bit 0 at offset 9 is set when the keyboard is attached;
* offset 19 contains the 8-bit sum of bytes 0 through 18.

Asynchronous report ``0x26`` also contains an event code at offset 11:
``0x02`` means detach and ``0x03`` means attach.  The driver derives state from
the status bit rather than the event code, so current-state replies and later
events use one validation and reporting path.
