#pragma once

// Port of NFSU2Unity Core/Audio/EaBnk.cs (the game-path decode only: EA-XA v2 and plain PCM; the vgmstream
// int16 decoders of EaXaDecoder.cs are not ported - the engine and shift banks of U2 PC are all EA-XA v2).

#include "drive/audio/EaPatchHeader.h"
#include "drive/audio/PcmSound.h"

#include <memory>
#include <vector>

namespace bb {

// One sound slot of a BNKl bank (Header == null for empty slots).
struct EaBnkSound {
    int Index = 0;
    // Absolute offset of the PT header.
    int HeaderOffset = 0;
    std::shared_ptr<EaPatchHeader> Header;
    // Absolute offset of each channel's data.
    std::vector<int> DataOffsets;

    bool IsEmpty() const { return Header == nullptr; }
};

// EA "BNKl" sound bank (sx.exe, version 4/5 layout), embedded in .abk files:
// 0x00 'BNKl', 0x04 u8 version (5), 0x05 u8 pad, 0x06 u16 sound count, 0x08 u32 bank size,
// 0x0C u32 (0 / file size), 0x10 u32 0, 0x14 u32 rel[count] (offset of each PT header relative to the
// entry itself; 0 = empty slot). Sample data offsets (PT 0x88..) are relative to the BNKl start.
// Keeps a pointer to the bytes: the owner (AbkBank) keeps them alive.
class EaBnk {
public:
    const std::vector<uint8_t>* Data = nullptr;
    int Offset = 0;
    int Version = 0;
    int Size = 0;
    std::vector<EaBnkSound> Sounds;

    EaBnk(const std::vector<uint8_t>& data, int offset);

    // Decodes sound index (flat, non-blocked layout); PCM codecs only (see the file comment). Null when empty.
    std::unique_ptr<PcmSound> Decode(int index) const;

    // Decodes sound index with the game's own float EA-XA decoder (SND sub_734AFF).
    // Falls back to Decode for codecs without a game-path decoder. Null when the slot is empty.
    std::unique_ptr<PcmSound> DecodeGame(int index) const;

    // Decodes a bank-style (non-blocked) sound: PCM codecs only; throws std::runtime_error for the others.
    static std::unique_ptr<PcmSound> DecodeFlat(const std::vector<uint8_t>& d, const EaPatchHeader& h, int baseOffset,
                                                const std::string& name);
};

} // namespace bb
