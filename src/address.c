#include "internal.h"
#include "base58.h"
#include <stdbool.h>
#include "ccan/ccan/build_assert/build_assert.h"
#include <include/wally_address.h>
#include <include/wally_bip32.h>
#include <include/wally_crypto.h>
#include <include/wally_script.h>

int wally_bip32_key_to_address(const struct ext_key *hdkey, uint32_t flags,
                               uint32_t version, char **output)
{
    unsigned char address[HASH160_LEN + 1];
    int ret;

    if (output)
        *output = NULL;

    if (!hdkey || (version & ~0xff) || (flags & ~0xff) || !output)
        return WALLY_EINVAL;

    if (!(flags & (WALLY_ADDRESS_TYPE_P2PKH | WALLY_ADDRESS_TYPE_P2SH_P2WPKH)))
        return WALLY_EINVAL;

    // Catch known incorrect combinations of address type and version:
    if ((flags & WALLY_ADDRESS_TYPE_P2PKH && version == WALLY_ADDRESS_VERSION_P2SH_MAINNET) ||
        (flags & WALLY_ADDRESS_TYPE_P2SH_P2WPKH && version == WALLY_ADDRESS_VERSION_P2PKH_MAINNET) ||
        (flags & WALLY_ADDRESS_TYPE_P2PKH && version == WALLY_ADDRESS_VERSION_P2SH_TESTNET) ||
        (flags & WALLY_ADDRESS_TYPE_P2SH_P2WPKH && version == WALLY_ADDRESS_VERSION_P2PKH_TESTNET))
        return WALLY_EINVAL;

    if (flags == WALLY_ADDRESS_TYPE_P2PKH) {
        // pub_key_hash = ripemd160(sha256(pubkey)
        address[0] = (unsigned char) version & 0xff;
        if (wally_hash160(hdkey->pub_key, sizeof(hdkey->pub_key), address + 1, HASH160_LEN) != WALLY_OK)
            return WALLY_EINVAL;
    } else {
        // redeem_script = SegWit version 0 + push(keyhash) = OP_0 + 0x20 + [key_hash]
        // where key_hash = ripemd160(sha256(pubkey))
        unsigned char redeem_script[HASH160_LEN + 2];
        redeem_script[0] = OP_0;
        redeem_script[1] = HASH160_LEN;

        if (wally_hash160(hdkey->pub_key, sizeof(hdkey->pub_key), redeem_script + 2, HASH160_LEN) != WALLY_OK)
            return WALLY_EINVAL;

        // P2SH address = version (e.g. 0x05) + ripemd160(sha256(redeem_script))
        address[0] = (unsigned char) version & 0xff;
        if (wally_hash160(redeem_script, sizeof(redeem_script), address + 1, HASH160_LEN) != WALLY_OK)
            return WALLY_EINVAL;
    }

    ret = wally_base58_from_bytes(address, sizeof(address), BASE58_FLAG_CHECKSUM, output);

    wally_clear(address, sizeof(address));
    return ret;
}

int wally_bip32_key_to_addr_segwit(const struct ext_key *hdkey, const char *addr_family,
                                   uint32_t flags, char **output)
{
    int ret;

    // Witness program bytes, including the version and data push opcode.
    unsigned char witness_program_bytes[HASH160_LEN + 2];
    witness_program_bytes[0] = OP_0;
    witness_program_bytes[1] = HASH160_LEN;

    if (wally_hash160(hdkey->pub_key, sizeof(hdkey->pub_key), witness_program_bytes + 2, HASH160_LEN) != WALLY_OK)
        return WALLY_EINVAL;

    ret = wally_addr_segwit_from_bytes(witness_program_bytes, HASH160_LEN + 2, addr_family, flags, output);
    return ret;
}

static bool is_p2pkh(unsigned char version)
{
   return version == WALLY_ADDRESS_VERSION_P2PKH_MAINNET ||
          version == WALLY_ADDRESS_VERSION_P2PKH_TESTNET ||
          version == WALLY_ADDRESS_VERSION_P2PKH_LIQUID ||
          version == WALLY_ADDRESS_VERSION_P2PKH_LIQUID_REGTEST;
}

static bool is_p2sh(unsigned char version)
{
   return version == WALLY_ADDRESS_VERSION_P2SH_MAINNET ||
          version == WALLY_ADDRESS_VERSION_P2SH_TESTNET ||
          version == WALLY_ADDRESS_VERSION_P2SH_LIQUID ||
          version == WALLY_ADDRESS_VERSION_P2SH_LIQUID_REGTEST;
}

static int network_from_addr_version(uint32_t version, uint32_t* network)
{
    switch (version) {
    case WALLY_ADDRESS_VERSION_P2PKH_MAINNET:
    case WALLY_ADDRESS_VERSION_P2SH_MAINNET:
        *network = WALLY_NETWORK_BITCOIN_MAINNET;
        break;
    case WALLY_ADDRESS_VERSION_P2PKH_TESTNET:
    case WALLY_ADDRESS_VERSION_P2SH_TESTNET:
        *network = WALLY_NETWORK_BITCOIN_TESTNET;
        break;
    case WALLY_ADDRESS_VERSION_P2PKH_LIQUID:
    case WALLY_ADDRESS_VERSION_P2SH_LIQUID:
        *network = WALLY_NETWORK_LIQUID;
        break;
    case WALLY_ADDRESS_VERSION_P2PKH_LIQUID_REGTEST:
    case WALLY_ADDRESS_VERSION_P2SH_LIQUID_REGTEST:
        *network = WALLY_NETWORK_LIQUID_REGTEST;
        break;
    default:
        return WALLY_EINVAL;
    }
    return WALLY_OK;
}

int wally_address_to_scriptpubkey(const char *addr, uint32_t network, unsigned char *bytes_out,
                                  size_t len, size_t *written)
{
    uint32_t version, addr_network;
    unsigned char bytes_base58_decode[1 + HASH160_LEN + BASE58_CHECKSUM_LEN];
    size_t written_base58_decode;

    if (written)
        *written = 0;

    // This returns WALLY_OK even if addr is too long for bytes_base58_decode
    if (wally_base58_to_bytes(addr, BASE58_FLAG_CHECKSUM, bytes_base58_decode, sizeof(bytes_base58_decode), &written_base58_decode) != WALLY_OK)
        return WALLY_EINVAL;

    if (written_base58_decode != HASH160_LEN + 1)
        return WALLY_EINVAL;

    version = bytes_base58_decode[0];
    if (network_from_addr_version(version, &addr_network) != WALLY_OK)
        return WALLY_EINVAL;
    if (network != addr_network)
        return WALLY_EINVAL;

    if (is_p2pkh(version)) {
        return wally_scriptpubkey_p2pkh_from_bytes(bytes_base58_decode + 1, HASH160_LEN, 0, bytes_out, len, written);
    } else if (is_p2sh(version)) {
        return wally_scriptpubkey_p2sh_from_bytes(bytes_base58_decode + 1, HASH160_LEN, 0, bytes_out, len, written);
    } else {
        return WALLY_EINVAL;
    }

}
