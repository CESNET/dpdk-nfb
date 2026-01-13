..  SPDX-License-Identifier: BSD-3-Clause
    Copyright 2019 Cesnet
    Copyright 2019 Netcope Technologies

NFB Poll Mode Driver
====================

The NFB PMD implements support for the FPGA-based
programmable NICs running `CESNET-NDK <https://www.liberouter.org/ndk/>`_
based firmware (formerly known as the NetCOPE platform).
The CESNET Network Development Kit offers
wide spectrum of supported cards, for example:
N6010, FB2CGG3 (Silicom Denmark),
IA-420F, IA-440i (BittWare),
AGI-FH400G (ReflexCES),
and `many more <https://github.com/CESNET/ndk-fpga/tree/devel/cards>`_.

The CESNET-NDK framework is open source and
can be found on `CESNET-NDK GitHub <https://github.com/CESNET/ndk-fpga>`_.
Ready-to-use demo firmwares can be found
on the `DYNANIC page <https://dyna-nic.com/try-now/>`_.

Software compatibility and firmware for
`historical cards <https://www.liberouter.org/technologies/cards/>`_
are left unmaintained.

Software prerequisites
----------------------

This PMD requires a Linux kernel module,
which is responsible for initialization and allocation of resources
needed for the nfb layer function.
Communication between PMD and kernel modules is mediated by the libnfb library.
The kernel module and library are not part of DPDK and must be installed separately.
Dependencies can be found on GitHub:
`nfb-framework <https://github.com/CESNET/ndk-sw>`_ as source code,
or for RPM-based distributions, the prebuilt `nfb-framework` package on
`Fedora Copr <https://copr.fedorainfracloud.org/coprs/g/CESNET/nfb-framework/>`_.

Before starting the DPDK, make sure that the kernel module is loaded (`sudo modprobe nfb`)
and the card is running the CESNET-NDK based firmware (`nfb-info -l`).

.. note::

   Currently, the driver is supported only on x86_64 architectures.

NFB card architecture
---------------------

Ethernet Ports
~~~~~~~~~~~~~~

The NFB cards are multi-port multi-queue cards,
where (generally) data from any Ethernet port may be sent by the firmware
to any queue.

The cards were historically represented in DPDK as a single port.
Currently each Ethernet channel is represented as one DPDK port.

.. note::

   Normally, one port corresponds to one channel,
   but ports can often be configured in a separate manner.
   For example one 100G port can be used as 4x25G or 4x10G independent Ethernet channels.

By default, all ports are initialized and used for the allowed PCI device.
When this behaviour is limiting
(e.g., for multiple instances of DPDK app on different ports of the same PCI device),
ports can be specified by the `port` item in the `allow` argument:

.. code-block:: console

   -a 0000:01:00.0,port=0,port=3

PCIe slots
~~~~~~~~~~

Some cards employ more than one PCIe device for better data throughput.
This can be achieved by slot bifurcation (only a minor improvement)
or by an add-on cable connected to another PCIe slot.
Both improvements can work together, as is,
for example, in the case of the AGI-FH400G card.

Because primary and secondary slot(s) can be attached to different NUMA nodes
(also applies for bifurcation on some HW),
the data structures need to be correctly allocated.
(Device-aware allocation matters also on IOMMU-enabled systems.)
The firmware already provides DMA queue to PCI device mapping.
The DPDK application just needs to use all PCI devices,
otherwise some queues will not be available;
provide all PCI endpoints listed in the `nfb-info -v` in the `allow` argument.

.. note::

   For cards where the number of Ethernet ports is less than the number of PCI devices
   (e.g., AGI-FH400G: 1 port, up to 4 PCI devices), the virtual DPDK ports are
   created to achieve the best NUMA-aware throughput
   (virtual ports lack a lot of configuration features).

Features
--------

Queue drivers
~~~~~~~~~~~~~

The DPDK NFB driver enables the user to choose the queue driver implementation.

The Native driver is the default and provides the best throughput parameters. It prepares
descriptors and communicates directly with the DMA controller in firmware with minimal overhead.

The NDP driver copies data from mbuf to NDP buffers allocated in kernels (and vice versa).
It is not optimal for throughput, but allows the user to run more applications
on the same DMA controller simultaneously.

The driver is selected with the help of the `queue_driver` item of the `allow` argument as follows:

.. code-block:: console

  -a 0000:01:00.0,queue_driver=native

.. code-block:: console

  -a 0000:01:00.0,queue_driver=ndp

.. note::

   For the best throughput of native driver, it is suitable to use more mempools for RX queues.
   This can maximize usage of compressed 64b descriptors over full 128b (2x64b) descriptors
   per mbuf, as the bits 64-30 of mbuf address vary minimally or not at all.


Timestamps
~~~~~~~~~~

The PMD supports hardware timestamps of frame receipt on physical network interface.
In order to use the timestamps, the hardware timestamping unit must be enabled
(follow the documentation of the NFB products).
The standard `RTE_ETH_RX_OFFLOAD_TIMESTAMP` flag can be used for this feature.

When the timestamps are enabled, a timestamp validity flag is set in the MBUFs
containing received frames and timestamp is inserted into the `rte_mbuf` struct.

The timestamp is an `uint64_t` field and holds the number of nanoseconds
elapsed since 1.1.1970 00:00:00 UTC.


Global RETA index
~~~~~~~~~~~~~~~~~

The PMD supports RSS with RETA, which is configurable for every port.
Default configuration routes data only to queues associated with the corresponding port.
Firmware with a crossbar allows to mixing traffic between queues of different ports,
which is helpful for processing biflows.

To enable this feature, use the `reta_index_global` item of the `allow` argument:

.. code-block:: console

   -a 0000:01:00.0,reta_index_global=1

and then set the target queue ID used in the RETA table,
not as a port-wide but as a card-wide number.

Custom Rx packet headers
~~~~~~~~~~~~~~~~~~~~~~~~

Most of the metadata parsed from the packet header is filled into mbuf:
RSS hash, VLAN, IP checksum, L4 checksum, packet type.

NDK-based firmware can provide a custom header (metadata) format, which is available
for the DPDK application via dynfield and needs to be enabled first:

.. code-block:: console

   -a 0000:01:00.0,rxhdr_dynfield=1

This allows the user to lookup for dynflag/dynfields:

 - `rte_net_nfb_dynflag_header_vld`: bit number in mbuf->ol_flags;
   bit is set when the custom header is valid;

 - `rte_net_nfb_dynfield_header_offset`: 16b offset from mbuf->buf_addr;
   where the custom header is located;

 - `rte_net_nfb_dynfield_header_len`: 16b header length;
 - `rte_net_nfb_dynfield_ndp_flags`: currently for internal use.

Then this approach can be used in the application (check for value validity!):

.. code-block:: c

   struct my_header {
      ...
   };
   int hdr_vld = mbuf->ol_flags &
         RTE_BIT64(rte_mbuf_dynflag_lookup("rte_net_nfb_dynfield_header_vld", NULL));
   int hdr_off = rte_mbuf_dynfield_lookup("rte_net_nfb_dynfield_header_offset", NULL)
   struct my_header *hdr = (struct my_header *)
         (mbuf->buf_addr + *RTE_MBUF_DYNFIELD(mbuf, hdr_off, uint16_t*));

Simulation
~~~~~~~~~~

The CESNET-NDK framework offers the possibility of simulating the firmware together with DPDK.
This allows for easy debugging of a packet flow behaviour with a specific firmware configuration.
The DPDK NFB driver can be connected to the simulator (Questa/ModelSim/nvc) via a virtual device:

.. code-block:: console

   dpdk-testpmd
      --vdev=eth_vdev_nfb,dev=libnfb-ext-grpc.so:grpc+dma_vas:localhost:50051,queue_driver=native
      --iova-mode=va -- -i

More info about the simulation can be found int the CESNET-NDK `documentation
<https://cesnet.github.io/ndk-fpga/devel/ndk_apps/minimal/tests/cocotb/readme.html>`_.
