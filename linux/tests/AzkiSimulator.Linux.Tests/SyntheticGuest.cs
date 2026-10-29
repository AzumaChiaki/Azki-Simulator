using System.Buffers.Binary;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// A synthetic BEST1503 AP image that actually boots on the emulator and drives
/// the modelled LCDC, plus the complete OTA that carries it.
///
/// The repository ships no firmware, so the boot ladder below could only ever be
/// written down and never run. This fixture closes that gap for the part the
/// Linux port owns: the guest is not a Xiaomi build and proves nothing about the
/// real firmware's behaviour, but it makes the host side — bwrap launch, the
/// runner's board geometry, the shared frame file, the bridge, the screenshot
/// path and process-group cleanup — execute end to end instead of being assumed.
///
/// Layout, in AP-file offsets (the emulator loads the file at flash base plus
/// the AP offset the header declares, and enters it at file offset 0x10):
///   0x00 header prefix; the address alias the probe reads lives at 0x0c
///   0x10 boot stub: ldr r0,[pc,#8] / msr msp,r0 / b .+6, then the stack literal
///   0x20 machine code, assembled from ARMv8-M Mainline (Cortex-M33) source
///   0x200 build_info text the static probe parses (CHIP, OTA_CODE_OFFSET, ...)
///   after it: 116-byte MTD partition records the board profile requires
/// </summary>
internal static class SyntheticGuest
{
    public const int ApBytes = 0x2000;
    private const int BuildInfoOffset = 0x200;

    private const string BuildInfo =
        "\nCHIP=best1503\nOTA_CODE_OFFSET=0xc0000\nFLASH_SIZE=0x1000000\n" +
        "VERSION=4.100.177\nMODEL=miwear.watch.q66tc\n";

    // Cortex-M33 Thumb-2, assembled from the listing below with llvm-mc
    // (--triple=thumbv8.1m.main-none-eabi -mcpu=cortex-m33) and stripped of the
    // 0x10-byte stub. Every address it touches comes from the emulator source:
    //   GPIO0/1/2 = 0x40081000/0x40089000/0x4008a000, LCDC = 0x40200000,
    //   PL011 UART0 = 0x4000b000, frame buffer = 0x20010000 in SRAM.
    // Sequence: power and select the panel (GPIO SET/CLR banks), program the
    // LCDC geometry (0x104/0x108/0x118 dimensions, 0xfc pitch, 0xf4 address,
    // 0x100 origin, 0x210 bit29 generator, 0x264 RGB565 enable), then forever:
    // paint 212x520 RGB565, clear status bit27 (0x1c4), start the transfer
    // (0x224 = 1), wait for 0x224 to fall and bit27 to rise, and emit one UART
    // byte per frame so the session log grows.
    //
    //   movw r4,#0x1000; movt r4,#0x4008   ; GPIO0
    //   mov.w r5,#0x01800000               ; panel power pins (banks 0/2 masks)
    //   str r5,[r4]; str r5,[r4,#4]        ; DATA set, OUTPUT-ENABLE set
    //   ... GPIO2 (0x4008a000) bit12, GPIO1 (0x40089000) CS bit9 output-low
    //   ... LCDC configuration, then the paint/start/wait loop
    private const string Code =
        "41f20004c4f208044ff0c075256065604af20004c4f2080441f2000525606560" +
        "49f20004c4f2080440f200256560e56040f20004c4f2200440f2d405c0f20825" +
        "c4f80451c4f80851c4f818514ff00005c4f8005140f2a815c4f8fc5040f20005" +
        "c2f20105c4f8f45040f20005c2f20005c4f8105240f20105c4f8645240f20008" +
        "c0f600084bf20006c4f200064df230794ff0000a514601f1400140f20002c2f2" +
        "01024b4642f8041b01f101015b1ef9d145466fea0505c4f8c45140f20105c4f8" +
        "2452d4f82452002dfbd1d4f8c45115ea080ffad0504600f03f0000f120003060" +
        "0af1010ad6e7";

    /// <summary>The AP image: header the probe reads, code the core executes,
    /// partition records the board profile reads.</summary>
    public static byte[] ApImage()
    {
        var data = new byte[ApBytes];
        // The probe recovers the load alias from the word at 0x0c, less the
        // offset of "CHIP=" in the build_info block, and requires a flash alias.
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x0c), 0x2800_0000u + BuildInfoOffset);
        // Boot stub. The reset vector lands on file offset 0x10, so this is the
        // entry point; the stack literal sits at 0x14 + 2*4 exactly as the
        // static probe computes it.
        Span<ushort> stub = [0x4802, 0xF380, 0x8808, 0xE003, 0xBF00, 0xBF00, 0xFE_C0, 0x2015];
        for (var index = 0; index < stub.Length; index++)
            BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(0x10 + 2 * index), stub[index]);
        var code = Convert.FromHexString(Code);
        Array.Copy(code, 0, data, 0x20, code.Length);
        var info = System.Text.Encoding.ASCII.GetBytes(BuildInfo);
        Array.Copy(info, 0, data, BuildInfoOffset, info.Length);
        var table = (BuildInfoOffset + info.Length + 3) & ~3;
        // The board profile derives the writable NOR tail from these records and the
        // emulator only accepts a geometry that is in its catalog, so this partition
        // map is the Q66 row of bes_nor_layout_init: AP at 0xc0000, a 288 KiB tail
        // from 0xfb8000, factory sector at 0xfc0000. mode is 64 sectors so that the
        // facmode record is the only one named in the filter, the three tile
        // contiguously, and they end exactly at the flash boundary.
        MtdRecord(data, table, "mode", 32192, 64);
        MtdRecord(data, table + 116, "facmode", 32256, 256);
        MtdRecord(data, table + 232, "nv", 32512, 256);
        // The NAND data partition the runner sizes its instance transaction by.
        MtdRecord(data, table + 348, "nand_data", 65536, 131072);
        return data;
    }

    private static void MtdRecord(byte[] data, int offset, string name, uint start, uint count)
    {
        System.Text.Encoding.ASCII.GetBytes(name).CopyTo(data.AsSpan(offset));
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(offset + 104), start);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(offset + 108), count);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(offset + 112), 0);
    }

    /// <summary>Writes a complete OTA package carrying <see cref="ApImage"/> and
    /// a ROMFS resource image, and returns its path.</summary>
    public static string WriteOta(string directory)
    {
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "synthetic-ota.zip");
        using var archive = new System.IO.Compression.ZipArchive(
            File.Create(path), System.IO.Compression.ZipArchiveMode.Create);
        Add(archive, "vela_ap.bin", ApImage());
        // The emulator only accepts a resource image that starts with the ROMFS
        // magic; nothing in the guest reads its contents.
        var resource = new byte[512];
        System.Text.Encoding.ASCII.GetBytes("-rom1fs-").CopyTo(resource, 0);
        Add(archive, "vela_resource.bin", resource);
        Add(archive, "version.ini", System.Text.Encoding.ASCII.GetBytes("sw_version=4.100.177\n"));
        return path;
    }

    private static void Add(System.IO.Compression.ZipArchive archive, string name, byte[] payload)
    {
        var entry = archive.CreateEntry(name);
        using var stream = entry.Open();
        stream.Write(payload, 0, payload.Length);
    }
}
