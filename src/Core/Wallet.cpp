// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <boost/algorithm/string.hpp>
#include "CryptoNoteTools.hpp"
#include "TransactionBuilder.hpp"
#include "WalletSerializationV1.hpp"
#include "WalletState.hpp"
#include "common/BIPs.hpp"
#include "common/CRC32.hpp"
#include "common/Math.hpp"
#include "common/MemoryStreams.hpp"
#include "common/StringTools.hpp"
#include "common/Varint.hpp"
#include "common/Words.hpp"
#include "crypto/crypto.hpp"
#include "http/JsonRpc.hpp"
#include "platform/Files.hpp"
#include "platform/PathTools.hpp"
#include "platform/Time.hpp"
#include "seria/BinaryInputStream.hpp"
#include "seria/BinaryOutputStream.hpp"

using namespace cn;

BinaryArray &operator|=(BinaryArray &a, const BinaryArray &b) {
	common::append(a, b);
	return a;
}

BinaryArray operator|(const BinaryArray &a, const BinaryArray &b) {
	BinaryArray tmp(a);
	return tmp |= b;
}

BinaryArray &operator|=(BinaryArray &a, const std::string &b) { return a |= common::as_binary_array(b); }

BinaryArray operator|(const BinaryArray &a, const std::string &b) {
	BinaryArray tmp(a);
	return tmp |= b;
}

BinaryArray &operator|=(BinaryArray &a, const Hash &b) {
	common::append(a, std::begin(b.data), std::end(b.data));
	return a;
}

BinaryArray operator|(const BinaryArray &a, const Hash &b) {
	BinaryArray tmp(a);
	return tmp |= b;
}

BinaryArray &operator|=(const Hash &b, BinaryArray &a) {
	common::append(a, std::begin(b.data), std::end(b.data));
	return a;
}

BinaryArray operator|(const Hash &a, const BinaryArray &b) {
	BinaryArray tmp(std::begin(a.data), std::end(a.data));
	return tmp |= b;
}
BinaryArray operator|(const Hash &a, const std::string &b) {
	BinaryArray tmp(std::begin(a.data), std::end(a.data));
	return tmp |= b;
}

static std::string net_append(const std::string &net) { return net == "main" ? std::string() : "_" + net + "net"; }

Wallet::Wallet(const Currency &currency, logging::ILogger &log, const std::string &path)
    : m_currency(currency), m_log(log, "Wallet"), m_path(path) {}

static Hash derive_from_seed_legacy(const Hash &seed, const std::string &append) {
	BinaryArray seed_data = common::as_binary_array(append);
	common::append(seed_data, std::begin(seed.data), std::end(seed.data));
	return crypto::cn_fast_hash(seed_data.data(), seed_data.size());
}

static Hash derive_from_seed(const Hash &seed, const std::string &append) {
	BinaryArray seed_data = seed | append;
	return crypto::cn_fast_hash(seed_data.data(), seed_data.size());
}

static Hash derive_from_key(const crypto::chacha_key &key, const std::string &append) {
	BinaryArray seed_data = BinaryArray(key.data, key.data + sizeof(key.data)) | append;
	common::append(seed_data, std::begin(key.data), std::end(key.data));
	return crypto::cn_fast_hash(seed_data.data(), seed_data.size());
}

AccountAddress Wallet::get_first_address() const { return record_to_address(m_wallet_records.at(0)); }

std::string Wallet::get_cache_name() const {
	Hash h           = crypto::cn_fast_hash(m_view_public_key.data, sizeof(m_view_public_key.data));
	std::string name = common::pod_to_hex(h);
	if (is_view_only()) {
		if (can_view_outgoing_addresses())
			name += "-view-only-voa";
		else
			name += "-view-only";
	}
	return name;
}

bool Wallet::get_look_ahead_record(WalletRecord &record, const PublicKey &spend_public_key) {
	auto rit = m_records_map.find(spend_public_key);
	if (rit == m_records_map.end())
		return false;
	invariant(m_wallet_records.at(rit->second).spend_public_key == spend_public_key, "");
	record = m_wallet_records.at(rit->second);
	create_look_ahead_records(rit->second + 1);
	return true;
}

static const uint8_t SERIALIZATION_VERSION_V2 = 6;

static const size_t CHECK_KEYS_COUNT = 128;  // >8 KB checked at start and end of file
#pragma pack(push, 1)
struct EncryptedWalletRecord {
	crypto::chacha_iv iv;
	// Secret key, public key and creation timestamp
	uint8_t data[sizeof(PublicKey) + sizeof(SecretKey) + sizeof(uint64_t)]{};
};
struct ContainerStoragePrefix {
	// We moved uint8_t version out of this struct, because with it other fields become unaligned
	crypto::chacha_iv next_iv;
	EncryptedWalletRecord encrypted_view_keys;
};
// struct ContainerStorageWalletRecord {
//	PublicKey pk{};
//	SecretKey sk{};
//	uint64_t ct = 0;
//};
#pragma pack(pop)

static void decrypt_key_pair(
    const EncryptedWalletRecord &r, PublicKey &pk, SecretKey &sk, Timestamp &ct, const crypto::chacha_key &key) {
	//	ContainerStorageWalletRecord rec;
	unsigned char rec_data[sizeof(r.data)]{};
	chacha8(r.data, sizeof(r.data), key, r.iv, rec_data);
	memcpy(pk.data, rec_data, sizeof(PublicKey));
	memcpy(sk.data, rec_data + sizeof(PublicKey), sizeof(SecretKey));
	ct = static_cast<Timestamp>(
	    common::uint_le_from_bytes<uint64_t>(rec_data + sizeof(PublicKey) + sizeof(SecretKey), sizeof(uint64_t)));
}

static void encrypt_key_pair(
    EncryptedWalletRecord &r, PublicKey pk, SecretKey sk, Timestamp ct, const crypto::chacha_key &key) {
	unsigned char rec_data[sizeof(r.data)]{};
	memcpy(rec_data, pk.data, sizeof(PublicKey));
	memcpy(rec_data + sizeof(PublicKey), sk.data, sizeof(SecretKey));
	common::uint_le_to_bytes<uint64_t>(rec_data + sizeof(PublicKey) + sizeof(SecretKey), sizeof(uint64_t), ct);
	r.iv = crypto::rand<crypto::chacha_iv>();
	chacha8(&rec_data, sizeof(r.data), key, r.iv, r.data);
}

bool Wallet::is_our_address(const AccountAddress &v_addr) const {
	WalletRecord wr;
	return get_record(&wr, v_addr);
}

size_t WalletContainerStorage::wallet_file_size(size_t records) {
	return 1 + sizeof(ContainerStoragePrefix) + sizeof(uint64_t) * 2 + sizeof(EncryptedWalletRecord) * records;
}

void WalletContainerStorage::load_container_storage() {
	uint8_t version = 0;
	ContainerStoragePrefix prefix{};
	unsigned char count_capacity_data[2 * sizeof(uint64_t)]{};
	m_file->read(&version, 1);
	m_file->read(&prefix, sizeof(prefix));
	m_file->read(count_capacity_data, 2 * sizeof(uint64_t));
	uint64_t f_item_capacity = common::uint_le_from_bytes<uint64_t>(count_capacity_data, sizeof(uint64_t));
	uint64_t f_item_count =
	    common::uint_le_from_bytes<uint64_t>(count_capacity_data + sizeof(uint64_t), sizeof(uint64_t));

	if (version < SERIALIZATION_VERSION_V2)
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Wallet version too old");

	Timestamp creation_timestamp = 0;  // We ignore view keys timestamp on load
	decrypt_key_pair(
	    prefix.encrypted_view_keys, m_view_public_key, m_view_secret_key, creation_timestamp, m_wallet_key);
	if (!keys_match(m_view_secret_key, m_view_public_key))
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Restored view public key doesn't correspond to secret key");

	const size_t item_count =
	    common::integer_cast<size_t>(std::min(f_item_count, f_item_capacity));  // Protection against write shredding
	if (item_count > std::numeric_limits<size_t>::max() / sizeof(EncryptedWalletRecord))
		throw Exception(
		    api::WALLET_FILE_DECRYPT_ERROR, "Restored item count is too big " + common::to_string(item_count));
	std::vector<EncryptedWalletRecord> all_encrypted(item_count);
	m_file->read(reinterpret_cast<char *>(all_encrypted.data()), sizeof(EncryptedWalletRecord) * item_count);
	bool tracking_mode = false;
	m_wallet_records.reserve(item_count);
	for (size_t i = 0; i != item_count; ++i) {
		WalletRecord wallet_record;
		decrypt_key_pair(all_encrypted[i], wallet_record.spend_public_key, wallet_record.spend_secret_key,
		    wallet_record.creation_timestamp, m_wallet_key);

		if (i == 0) {
			tracking_mode = wallet_record.spend_secret_key == SecretKey{};
		} else if ((tracking_mode && wallet_record.spend_secret_key != SecretKey{}) ||
		           (!tracking_mode && wallet_record.spend_secret_key == SecretKey{})) {
			throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "All addresses must be either tracking or not");
		}

		if (i < CHECK_KEYS_COUNT || i >= item_count - CHECK_KEYS_COUNT) {  // check only first and last spend keys
			if (wallet_record.spend_secret_key != SecretKey{}) {
				if (!keys_match(wallet_record.spend_secret_key, wallet_record.spend_public_key))
					throw Exception(
					    api::WALLET_FILE_DECRYPT_ERROR, "Restored spend public key doesn't correspond to secret key");
			} else {
				if (!key_isvalid(wallet_record.spend_public_key)) {
					throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Public spend key is incorrect");
				}
			}
		}
		m_oldest_timestamp = std::min(m_oldest_timestamp, wallet_record.creation_timestamp);
		m_records_map.insert(std::make_pair(wallet_record.spend_public_key, m_wallet_records.size()));
		m_wallet_records.push_back(wallet_record);
	}
	auto file_size           = m_file->seek(0, SEEK_END);
	auto should_be_file_size = wallet_file_size(item_count);
	if (file_size > should_be_file_size) {  // We truncate legacy wallet cache
		try {
			m_file->truncate(should_be_file_size);
			m_log(logging::WARNING) << "Truncated wallet cache legacy wallet file to size=" << should_be_file_size
			                        << std::endl;
		} catch (const std::exception &) {  // probably read only, ignore
		}
	}
}

void WalletContainerStorage::load_legacy_wallet_file() {
	//	m_wallet_records.clear();
	//	std::vector<WalletRecord> wallets_container;

	WalletSerializerV1 s(m_view_public_key, m_view_secret_key, m_wallet_records);

	s.load(m_wallet_key, *m_file.get());

	//	m_wallet_records.reserve()
	//	m_first_record = wallets_container.at(0);
	for (size_t i = 0; i != m_wallet_records.size(); ++i) {
		m_oldest_timestamp = std::min(m_oldest_timestamp, m_wallet_records[i].creation_timestamp);
		m_records_map.insert(std::make_pair(m_wallet_records[i].spend_public_key, i));
	}
}

WalletContainerStorage::WalletContainerStorage(
    const Currency &currency, logging::ILogger &log, const std::string &path, const crypto::chacha_key &wallet_key)
    : Wallet(currency, log, path) {
	m_wallet_key = wallet_key;
	load();
}

WalletContainerStorage::WalletContainerStorage(const Currency &currency, logging::ILogger &log, const std::string &path,
    const std::string &password, const std::string &import_keys, Timestamp creation_timestamp)
    : Wallet(currency, log, path) {
	crypto::CryptoNightContext cn_ctx;
	m_wallet_key = generate_chacha8_key(cn_ctx, password.data(), password.size());
	//	try {
	m_file.reset(new platform::FileStream(path, platform::O_CREATE_NEW));
	//	} catch (const common::StreamError &) {
	// file does not exist
	//	}
	//	if (file.get())  // opened ok
	//		throw Exception(api::WALLET_FILE_EXISTS,
	//			"Will not overwrite existing wallet - delete it first or specify another file " + path);

	if (import_keys.empty()) {
		m_oldest_timestamp = platform::now_unix_timestamp();  // ignore creation_timestamp
		crypto::random_keypair(m_view_public_key, m_view_secret_key);
		m_wallet_records.push_back(WalletRecord{});
		m_wallet_records.at(0).creation_timestamp = m_oldest_timestamp;
		crypto::random_keypair(m_wallet_records.at(0).spend_public_key, m_wallet_records.at(0).spend_secret_key);
	} else {
		if (import_keys.size() != 256)
			throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Imported keys should be exactly 128 hex bytes");
		WalletRecord record{};
		record.creation_timestamp = creation_timestamp;
		if (!common::pod_from_hex(import_keys.substr(0, 64), &record.spend_public_key) ||
		    !common::pod_from_hex(import_keys.substr(64, 64), &m_view_public_key) ||
		    !common::pod_from_hex(import_keys.substr(128, 64), &record.spend_secret_key) ||
		    !common::pod_from_hex(import_keys.substr(192, 64), &m_view_secret_key))
			throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Imported keys should contain only hex bytes");
		if (!keys_match(m_view_secret_key, m_view_public_key))
			throw Exception(
			    api::WALLET_FILE_DECRYPT_ERROR, "Imported secret view key does not match corresponding public key");
		if (record.spend_secret_key != SecretKey{} && !keys_match(record.spend_secret_key, record.spend_public_key))
			throw Exception(
			    api::WALLET_FILE_DECRYPT_ERROR, "Imported secret spend key does not match corresponding public key");
		m_wallet_records.push_back(record);
		m_oldest_timestamp = 0;  // Alas, will scan entire blockchain
	}
	m_records_map.insert(std::make_pair(m_wallet_records.at(0).spend_public_key, 0));
	save_and_check();
	load();
}

WalletContainerStorage::WalletContainerStorage(
    const Currency &currency, logging::ILogger &log, const std::string &path, const std::string &password)
    : Wallet(currency, log, path) {
	crypto::CryptoNightContext cn_ctx;
	m_wallet_key = generate_chacha8_key(cn_ctx, password.data(), password.size());
	load();
}

void WalletContainerStorage::load() {
	try {
		m_file.reset(new platform::FileStream(m_path, platform::O_OPEN_EXISTING));
	} catch (const common::StreamError &) {  // Read-only media?
		m_file.reset(new platform::FileStream(m_path, platform::O_READ_EXISTING));
	}
	uint8_t version = 0;
	m_file->read(&version, 1);
	if (version > SERIALIZATION_VERSION_V2)
		throw Exception(api::WALLET_FILE_UNKNOWN_VERSION, "Unknown version");
	m_file->seek(0, SEEK_SET);
	if (version < SERIALIZATION_VERSION_V2) {
		try {
			load_legacy_wallet_file();
		} catch (const common::StreamError &ex) {
			std::throw_with_nested(
			    Exception(api::WALLET_FILE_READ_ERROR, std::string("Error reading wallet file ") + common::what(ex)));
		} catch (const std::exception &ex) {
			std::throw_with_nested(Exception(
			    api::WALLET_FILE_DECRYPT_ERROR, std::string("Error decrypting wallet file ") + common::what(ex)));
		}
		m_file.reset();  // Indicates legacy format
		try {
			save_and_check();  // We try to overwrite legacy format with new format
			m_log(logging::WARNING) << "Overwritten legacy wallet file with new data format" << std::endl;
		} catch (const std::exception &) {  // probably read only, ignore
		}
	} else {
		load_container_storage();
	}
	if (m_wallet_records.empty())
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Error reading wallet file");

	if (!is_view_only()) {
		BinaryArray seed_data;
		seed_data.assign(std::begin(m_view_secret_key.data), std::end(m_view_secret_key.data));
		common::append(seed_data, std::begin(m_wallet_records.at(0).spend_secret_key.data),
		    std::end(m_wallet_records.at(0).spend_secret_key.data));
		m_seed                  = crypto::cn_fast_hash(seed_data.data(), seed_data.size());
		m_tx_derivation_seed    = derive_from_seed_legacy(m_seed, "tx_derivation");
		m_history_filename_seed = derive_from_seed_legacy(m_seed, "history_filename");
		m_history_key           = crypto::chacha_key{derive_from_seed_legacy(m_seed, "history")};
	}
}

void WalletContainerStorage::save(const std::string &export_path, const crypto::chacha_key &wallet_key, bool view_only,
    platform::OpenMode open_mode) const {
	platform::FileStream f(export_path, open_mode);

	uint8_t version = SERIALIZATION_VERSION_V2;
	ContainerStoragePrefix prefix{};
	encrypt_key_pair(prefix.encrypted_view_keys, m_view_public_key, m_view_secret_key, m_oldest_timestamp, wallet_key);
	unsigned char count_capacity_data[sizeof(uint64_t)]{};
	common::uint_le_to_bytes<uint64_t>(count_capacity_data, sizeof(uint64_t), m_wallet_records.size());
	f.write(&version, 1);
	f.write(&prefix, sizeof(prefix));

	f.write(count_capacity_data, sizeof(uint64_t));  // we set capacity to item_count
	f.write(count_capacity_data, sizeof(uint64_t));

	EncryptedWalletRecord record;
	for (const auto &rec : m_wallet_records) {
		encrypt_key_pair(record, rec.spend_public_key, view_only ? SecretKey{} : rec.spend_secret_key,
		    rec.creation_timestamp, wallet_key);
		f.write(&record, sizeof(record));
	}
	f.fsync();
}

std::string WalletContainerStorage::export_keys() const {
	BinaryArray result;
	common::append(result, std::begin(m_wallet_records.at(0).spend_public_key.data),
	    std::end(m_wallet_records.at(0).spend_public_key.data));
	common::append(result, std::begin(m_view_public_key.data), std::end(m_view_public_key.data));
	common::append(result, std::begin(m_wallet_records.at(0).spend_secret_key.data),
	    std::end(m_wallet_records.at(0).spend_secret_key.data));
	common::append(result, std::begin(m_view_secret_key.data), std::end(m_view_secret_key.data));
	return common::to_hex(result);
}

void WalletContainerStorage::save_and_check() {
	const std::string tmp_path = m_path + ".tmp";

	save(tmp_path, m_wallet_key, false, platform::O_CREATE_ALWAYS);

	WalletContainerStorage other(m_currency, m_log.get_logger(), tmp_path, m_wallet_key);
	if (*this != other)
		throw Exception(api::WALLET_FILE_WRITE_ERROR, "Error writing wallet file - records do not match");
	m_file.reset();
	if (!platform::atomic_replace_file(tmp_path, m_path))
		throw Exception(api::WALLET_FILE_WRITE_ERROR, "Error replacing wallet file");
	std::swap(m_file, other.m_file);
}

void WalletContainerStorage::set_password(const std::string &password) {
	crypto::CryptoNightContext cn_ctx;
	m_wallet_key = generate_chacha8_key(cn_ctx, password.data(), password.size());
	save_and_check();
}

void WalletContainerStorage::export_wallet(const std::string &export_path, const std::string &new_password,
    bool view_only, bool view_outgoing_addresses) const {
	std::unique_ptr<platform::FileStream> export_file;
	for (const auto &rec : m_wallet_records) {
		if (rec.spend_secret_key != SecretKey{}) {
			if (!keys_match(rec.spend_secret_key, rec.spend_public_key))
				throw Exception(api::WALLET_FILE_DECRYPT_ERROR,
				    "Spend public key doesn't correspond to secret key (corrupted wallet?)");
		} else {
			if (!key_isvalid(rec.spend_public_key)) {
				throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Public spend key is incorrect (corrupted wallet?)");
			}
		}
	}
	crypto::CryptoNightContext cn_ctx;
	auto new_wallet_key = generate_chacha8_key(cn_ctx, new_password.data(), new_password.size());
	save(export_path, new_wallet_key, view_only, platform::O_CREATE_NEW);
}

bool WalletContainerStorage::operator==(const WalletContainerStorage &other) const {
	return m_view_public_key == other.m_view_public_key && m_view_secret_key == other.m_view_secret_key &&
	       m_oldest_timestamp == other.m_oldest_timestamp && m_wallet_records == other.m_wallet_records;
}

std::vector<WalletRecord> WalletContainerStorage::generate_new_addresses(
    const std::vector<SecretKey> &sks, Timestamp ct, Timestamp now, bool *rescan_from_ct) {
	std::vector<WalletRecord> result;
	if (is_view_only())
		throw Exception(101, "Generate new addresses impossible for view-only wallet");
	if (!m_file.get()) {  // Legacy format, now overwrite
		m_log(logging::WARNING) << "Creation of new addresses forces overwrite of legacy format wallet" << std::endl;
		save_and_check();
	}
	*rescan_from_ct   = false;
	size_t append_pos = wallet_file_size(m_wallet_records.size());
	m_file->seek(append_pos, SEEK_SET);
	for (auto &&sk : sks) {
		WalletRecord record{};
		if (sk == SecretKey{}) {
			record.creation_timestamp = now;
			do {
				crypto::random_keypair(record.spend_public_key, record.spend_secret_key);
			} while (m_records_map.count(record.spend_public_key) != 0);
			m_oldest_timestamp = std::min(m_oldest_timestamp, record.creation_timestamp);
		} else {
			record.creation_timestamp = ct;
			record.spend_secret_key   = sk;
			if (!secret_key_to_public_key(sk, &record.spend_public_key))
				throw Exception(101, "Imported keypair is invalid - sk=" + common::pod_to_hex(sk));
		}
		auto rit = m_records_map.find(record.spend_public_key);
		if (rit != m_records_map.end()) {
			if (m_wallet_records.at(rit->second).creation_timestamp > record.creation_timestamp) {
				m_wallet_records.at(rit->second).creation_timestamp = record.creation_timestamp;
				m_oldest_timestamp = std::min(m_oldest_timestamp, record.creation_timestamp);
				*rescan_from_ct    = true;
			}
			result.push_back(m_wallet_records.at(rit->second));
			continue;
		}
		m_records_map.insert(std::make_pair(record.spend_public_key, m_wallet_records.size()));
		m_wallet_records.push_back(record);
		EncryptedWalletRecord enc_record;
		encrypt_key_pair(
		    enc_record, record.spend_public_key, record.spend_secret_key, record.creation_timestamp, m_wallet_key);
		m_file->write(&enc_record, sizeof(enc_record));
		result.push_back(record);
	}
	m_file->fsync();
	m_file->seek(1 + sizeof(ContainerStoragePrefix), SEEK_SET);

	unsigned char count_capacity_data[sizeof(uint64_t)]{};
	common::uint_le_to_bytes<uint64_t>(count_capacity_data, sizeof(uint64_t), m_wallet_records.size());

	m_file->write(count_capacity_data, sizeof(uint64_t));
	m_file->write(count_capacity_data, sizeof(uint64_t));

	m_file->fsync();
	if (*rescan_from_ct) {  // We never write to the middle of the file
		m_log(logging::WARNING) << "Updating creation timestamp of existing addresses to " << ct
		                        << " in a wallet file (might take minutes for large wallets)..." << std::endl;
		save_and_check();
	}
	return result;
}

AccountAddress WalletContainerStorage::record_to_address(const WalletRecord &record) const {
	return AccountAddressSimple{record.spend_public_key, m_view_public_key};
}

bool WalletContainerStorage::get_record(WalletRecord *record, const AccountAddress &v_addr) const {
	if (v_addr.type() != typeid(AccountAddressSimple))
		return false;
	auto &addr = boost::get<AccountAddressSimple>(v_addr);
	auto rit   = m_records_map.find(addr.spend_public_key);
	if (m_view_public_key != addr.view_public_key || rit == m_records_map.end())
		return false;
	if (rit->second >= get_actual_records_count())
		return false;
	invariant(m_wallet_records.at(rit->second).spend_public_key == addr.spend_public_key, "");
	*record = m_wallet_records.at(rit->second);
	return true;
}

void WalletContainerStorage::on_first_output_found(Timestamp ts) {
	if (m_currency.net != "main")
		return;  // Legacy format has not place for other nets
	if (ts == 0 || m_oldest_timestamp != 0)
		return;
	m_oldest_timestamp = ts;
	for (auto &&rec : m_wallet_records)
		if (rec.creation_timestamp == 0)
			rec.creation_timestamp = ts;
	m_log(logging::WARNING) << "Updating creation timestamp to " << ts
	                        << " in a wallet file (might take minutes for large wallets)..." << std::endl;
	save_and_check();
}

void WalletContainerStorage::backup(const std::string &dst_name, const std::string &pass) const {
	const std::string dst_history_name  = dst_name + ".history";
	const std::string dst_payments_name = dst_name + ".payments";
	if (!platform::create_folder_if_necessary(dst_payments_name))
		throw std::runtime_error("Could not create folder for backup " + dst_payments_name);
	if (!platform::create_folder_if_necessary(dst_history_name))
		throw std::runtime_error("Could not create folder for backup " + dst_history_name);
	export_wallet(dst_name, pass, false, false);
	for (const auto &file : platform::get_filenames_in_folder(get_payment_queue_folder())) {
		platform::copy_file(get_payment_queue_folder() + "/" + file, dst_payments_name + "/" + file);
	}
	for (const auto &file : platform::get_filenames_in_folder(get_history_folder())) {
		platform::copy_file(get_history_folder() + "/" + file, dst_history_name + "/" + file);
	}
}

std::string WalletContainerStorage::get_history_folder() const {
	return m_path + ".history" + net_append(m_currency.net);
}

std::string WalletContainerStorage::get_payment_queue_folder() const {
	return m_path + ".payments" + net_append(m_currency.net);
}

bool WalletContainerStorage::save_history(const Hash &tid, const History &used_addresses) {
	std::string history_folder = get_history_folder();
	if (!platform::create_folders_if_necessary(history_folder))
		return false;
	if (used_addresses.empty())
		return true;  // saved empty history :)
	crypto::chacha_iv iv = crypto::rand<crypto::chacha_iv>();
	BinaryArray data;

	for (auto &&addr : used_addresses) {
		common::append(data, std::begin(addr.view_public_key.data), std::end(addr.view_public_key.data));
		common::append(data, std::begin(addr.spend_public_key.data), std::end(addr.spend_public_key.data));
	}
	BinaryArray encrypted_data;
	encrypted_data.resize(data.size(), 0);
	crypto::chacha8(data.data(), data.size(), m_history_key, iv, encrypted_data.data());
	encrypted_data.insert(encrypted_data.begin(), std::begin(iv.data), std::end(iv.data));

	BinaryArray filename_data(std::begin(tid.data), std::end(tid.data));
	common::append(filename_data, std::begin(m_history_filename_seed.data), std::end(m_history_filename_seed.data));
	Hash filename_hash = crypto::cn_fast_hash(filename_data.data(), filename_data.size());

	const auto tmp_path = history_folder + "/_tmp.txh";
	return platform::atomic_save_file(history_folder + "/" + common::pod_to_hex(filename_hash) + ".txh",
	    encrypted_data.data(), encrypted_data.size(), tmp_path);
}

Wallet::History WalletContainerStorage::load_history(const Hash &tid) const {
	Wallet::History used_addresses;
	std::string history_folder = get_history_folder();
	BinaryArray filename_data(std::begin(tid.data), std::end(tid.data));
	common::append(filename_data, std::begin(m_history_filename_seed.data), std::end(m_history_filename_seed.data));
	Hash filename_hash = crypto::cn_fast_hash(filename_data.data(), filename_data.size());

	BinaryArray hist;
	if (!platform::load_file(history_folder + "/" + common::pod_to_hex(filename_hash) + ".txh", hist) ||
	    hist.size() < sizeof(crypto::chacha_iv) ||
	    (hist.size() - sizeof(crypto::chacha_iv)) % (2 * sizeof(PublicKey)) != 0)
		return used_addresses;
	const crypto::chacha_iv *iv = (const crypto::chacha_iv *)hist.data();
	BinaryArray dec(hist.size() - sizeof(crypto::chacha_iv), 0);
	crypto::chacha8(hist.data() + sizeof(crypto::chacha_iv), hist.size() - sizeof(crypto::chacha_iv), m_history_key,
	    *iv, dec.data());
	for (size_t i = 0; i != dec.size() / (2 * sizeof(PublicKey)); ++i) {
		AccountAddressSimple ad;
		memcpy(ad.view_public_key.data, dec.data() + i * 2 * sizeof(PublicKey), sizeof(PublicKey));
		memcpy(ad.spend_public_key.data, dec.data() + i * 2 * sizeof(PublicKey) + sizeof(PublicKey), sizeof(PublicKey));
		used_addresses.insert(ad);
	}
	return used_addresses;
}

std::vector<BinaryArray> WalletContainerStorage::payment_queue_get() const {
	std::vector<BinaryArray> result;
	platform::remove_file(get_payment_queue_folder() + "/tmp.tx");
	for (const auto &file : platform::get_filenames_in_folder(get_payment_queue_folder())) {
		common::BinaryArray body;
		if (!platform::load_file(get_payment_queue_folder() + "/" + file, body))
			continue;
		result.push_back(std::move(body));
	}
	return result;
}

void WalletContainerStorage::payment_queue_add(const Hash &tid, const BinaryArray &binary_transaction) {
	const std::string file = get_payment_queue_folder() + "/" + common::pod_to_hex(tid) + ".tx";
	platform::create_folder_if_necessary(get_payment_queue_folder());
	if (!platform::atomic_save_file(
	        file, binary_transaction.data(), binary_transaction.size(), get_payment_queue_folder() + "/tmp.tx"))
		m_log(logging::WARNING) << "Failed to save transaction " << tid << " to file " << file << std::endl;
	else
		m_log(logging::INFO) << "Saved transaction " << tid << " to file " << file << std::endl;
}

void WalletContainerStorage::payment_queue_remove(const Hash &tid) {
	const std::string file = get_payment_queue_folder() + "/" + common::pod_to_hex(tid) + ".tx";
	if (!platform::remove_file(file))
		m_log(logging::WARNING) << "Failed to remove PQ transaction " << tid << " from file " << file << std::endl;
	else
		m_log(logging::INFO) << "Removed PQ transaction " << tid << " from file " << file << std::endl;
	platform::remove_file(get_payment_queue_folder());  // When it becomes empty
}

void WalletContainerStorage::set_label(const std::string &address, const std::string &label) {
	throw std::runtime_error("Linkable wallet file cannot store labels");
}

Wallet::OutputHandler WalletContainerStorage::get_output_handler() const {
	SecretKey vsk_copy = m_view_secret_key;
	return
	    [vsk_copy](const PublicKey &tx_public_key, boost::optional<KeyDerivation> *kd, const Hash &tx_inputs_hash,
	        size_t output_index, const OutputKey &key_output, PublicKey *spend_public_key, SecretKey *secret_scalar) {
		    if (!*kd) {
			    try {
				    *kd = generate_key_derivation(tx_public_key, vsk_copy);
				    // tx_public_key is not checked by daemon, so can be invalid
			    } catch (const std::exception &) {
				    *kd = KeyDerivation{};
			    }
		    }
		    *spend_public_key = underive_public_key(kd->get(), output_index, key_output.public_key);
	    };
}

bool WalletContainerStorage::detect_our_output(const Hash &tid, const Hash &tx_inputs_hash,
    const boost::optional<KeyDerivation> &kd, size_t out_index, const PublicKey &spend_public_key,
    const SecretKey &secret_scalar, const OutputKey &key_output, Amount *amount, KeyPair *output_keypair,
    AccountAddress *address) {
	WalletRecord record;
	if (!get_look_ahead_record(record, spend_public_key))
		return false;
	AccountAddressSimple s_address{spend_public_key, get_view_public_key()};
	if (record.spend_secret_key != SecretKey{}) {
		if (!kd)  // tx_public_key was invalid
			return false;
		// We do some calcs twice here, but only for our outputs (which are usually very small %)
		output_keypair->public_key = derive_public_key(kd.get(), out_index, spend_public_key);
		output_keypair->secret_key = derive_secret_key(kd.get(), out_index, record.spend_secret_key);
		if (output_keypair->public_key != key_output.public_key)
			return false;
	}
	*address = s_address;
	*amount  = key_output.amount;
	return true;
}

static const std::string current_version = "CryptoNoteWallet1";
static const size_t GENERATE_AHEAD       = 20000;  // TODO - move to better place

std::string WalletHD::generate_mnemonic(size_t bits, uint32_t version) {
	using common::BITS_PER_WORD;
	using common::crc32_reverse_step_zero;
	using common::crc32_step_zero;
	using common::word_crc32_adj;
	using common::word_ptrs;
	using common::words_bylen;
	using common::WORDS_COUNT;
	using common::WORDS_MAX_LEN;
	using common::WORDS_MIN_LEN;
	std::unordered_map<uint32_t, size_t> last_word(WORDS_COUNT);
	for (size_t i = 0; i != WORDS_COUNT; i++) {
		uint32_t crc32_suffix = version ^ word_crc32_adj[i];
		for (auto p = word_ptrs[i]; p != word_ptrs[i + 1]; p++) {
			crc32_suffix = crc32_reverse_step_zero(crc32_suffix);
		}
		last_word[crc32_suffix] = i;
	}
	size_t words_in_prefix = (bits - 1) / BITS_PER_WORD + 1;
	size_t words_total     = words_in_prefix + 3;
	std::unique_ptr<size_t[]> word_ids(new size_t[words_total]);
	while (true) {
		uint32_t crc32_prefix = 0;
		for (size_t i = 0; i != words_in_prefix; i++) {
			size_t j    = crypto::rand<size_t>() % WORDS_COUNT;
			word_ids[i] = j;
			for (auto p = word_ptrs[j]; p != word_ptrs[j + 1]; p++) {
				crc32_prefix = crc32_step_zero(crc32_prefix);
			}
			crc32_prefix ^= word_crc32_adj[j];
		}
		for (size_t i = 0; i < WORDS_MIN_LEN; i++) {
			crc32_prefix = crc32_step_zero(crc32_prefix);
		}
		const uint32_t *adj1 = word_crc32_adj;
		for (size_t l1 = 0;; l1++) {
			for (; adj1 != words_bylen[l1]; adj1++) {
				uint32_t crc32_prefix2 = crc32_prefix ^ *adj1;
				for (size_t i = 0; i < WORDS_MIN_LEN; i++) {
					crc32_prefix2 = crc32_step_zero(crc32_prefix2);
				}
				const uint32_t *adj2 = word_crc32_adj;
				for (size_t l2 = 0;; l2++) {
					for (; adj2 != words_bylen[l2]; adj2++) {
						auto it = last_word.find(crc32_prefix2 ^ *adj2);
						if (it != last_word.end()) {
							word_ids[words_in_prefix]     = adj1 - word_crc32_adj;
							word_ids[words_in_prefix + 1] = adj2 - word_crc32_adj;
							word_ids[words_in_prefix + 2] = it->second;
							size_t word0                  = word_ids[0];
							std::string result(word_ptrs[word0], word_ptrs[word0 + 1]);
							for (size_t i = 1; i != words_total; i++) {
								result.push_back(' ');
								size_t word = word_ids[i];
								result.append(word_ptrs[word], word_ptrs[word + 1]);
							}
							return result;
						}
					}
					if (l2 == WORDS_MAX_LEN - WORDS_MIN_LEN) {
						break;
					}
					crc32_prefix2 = crc32_step_zero(crc32_prefix2);
				}
			}
			if (l1 == WORDS_MAX_LEN - WORDS_MIN_LEN) {
				break;
			}
			crc32_prefix = crc32_step_zero(crc32_prefix);
		}
	}
}

static const std::string ADDRESS_COUNT_PREFIX      = "total_address_count";
static const std::string CREATION_TIMESTAMP_PREFIX = "creation_timestamp";

using namespace platform;

bool WalletHD::is_sqlite(const std::string &full_path) {
	bool created = false;
	sqlite::Dbi db_dbi;
	try {
		db_dbi.open_check_create(platform::O_READ_EXISTING, full_path, &created);
		return true;
	} catch (const std::exception &) {
	}
	return false;
}

WalletHD::WalletHD(const Currency &currency, logging::ILogger &log, const std::string &path,
    const std::string &password, bool readonly)
    : Wallet(currency, log, path) {
	bool created = false;
	m_db_dbi.open_check_create(readonly ? platform::O_READ_EXISTING : platform::O_OPEN_EXISTING, path, &created);
	BinaryArray salt = get_salt();
	common::append(salt, common::as_binary_array(password));
	crypto::CryptoNightContext cn_ctx;
	m_wallet_key = generate_chacha8_key(cn_ctx, salt.data(), salt.size());
	try {
		load();
	} catch (const Bip32Key::Exception &) {
		std::throw_with_nested(Exception{api::WALLETD_MNEMONIC_CRC, "Wrong mnemonic"});
	} catch (const std::exception &) {
		std::throw_with_nested(Exception{api::WALLET_FILE_DECRYPT_ERROR, "Wallet file invalid or wrong password"});
	}
}

WalletHD::WalletHD(const Currency &currency, logging::ILogger &log, const std::string &path,
    const std::string &password, const std::string &mnemonic, uint8_t address_type, Timestamp creation_timestamp,
    const std::string &mnemonic_password)
    : Wallet(currency, log, path) {
	bool created = false;
	m_db_dbi.open_check_create(platform::O_CREATE_NEW, path, &created);
	m_db_dbi.exec(
	    "CREATE TABLE unencrypted(key BLOB PRIMARY KEY COLLATE BINARY NOT NULL, value BLOB NOT NULL) WITHOUT ROWID");
	m_db_dbi.exec(
	    "CREATE TABLE parameters(key_hash BLOB PRIMARY KEY COLLATE BINARY NOT NULL, key BLOB NOT NULL, value BLOB NOT NULL) WITHOUT ROWID");
	m_db_dbi.exec(
	    "CREATE TABLE labels(address_hash BLOB PRIMARY KEY NOT NULL, address BLOB NOT NULL, label BLOB NOT NULL) WITHOUT ROWID");
	m_db_dbi.exec(
	    "CREATE TABLE payment_queue(tid_hash BLOB COLLATE BINARY NOT NULL, net_hash BLOB COLLATE BINARY NOT NULL, tid BLOB NOT NULL, net BLOB NOT NULL, binary_transaction BLOB NOT NULL, PRIMARY KEY (tid_hash, net_hash)) WITHOUT ROWID");
	BinaryArray salt(sizeof(Hash));
	crypto::generate_random_bytes(salt.data(), salt.size());
	put_salt(salt);  // The only unencrypted field
	common::append(salt, common::as_binary_array(password));
	crypto::CryptoNightContext cn_ctx;
	m_wallet_key = generate_chacha8_key(cn_ctx, salt.data(), salt.size());

	if (mnemonic.empty())
		return;
	put("version", current_version, true);
	put("coinname", CRYPTONOTE_NAME, true);
	put("address-type", BinaryArray(1, address_type), true);
	put("mnemonic", cn::Bip32Key::check_bip39_mnemonic(mnemonic), true);
	put("mnemonic-password", mnemonic_password, true);  // write always to keep row count the same
	put(ADDRESS_COUNT_PREFIX, seria::to_binary(m_used_address_count), true);

	on_first_output_found(creation_timestamp);

	try {
		load();
	} catch (const Bip32Key::Exception &) {
		std::throw_with_nested(Exception{api::WALLETD_MNEMONIC_CRC, "Wrong mnemonic"});
	} catch (const std::exception &) {
		std::throw_with_nested(Exception{api::WALLET_FILE_DECRYPT_ERROR, "Wallet file invalid or wrong password"});
	}
	commit();
}

void WalletHD::load() {
	std::string version;
	if (!get("version", version) || version != current_version)
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Wallet version unknown - " + version);
	std::string coinname;
	if (!get("coinname", coinname) || coinname != CRYPTONOTE_NAME)
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Wallet is for different coin - " + coinname);
	std::string mnemonic;
	BinaryArray address_type;
	if (!get("address-type", address_type) || address_type.size() != 1)
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Wallet corrupted, no address type");
	m_address_type = address_type.at(0);
	if (m_address_type != AccountAddressUnlinkable::type_tag &&
	    m_address_type != AccountAddressUnlinkable::type_tag_auditable)
		throw Exception(api::WALLET_FILE_DECRYPT_ERROR, "Wallet address type unknown");
	if (get("mnemonic", mnemonic)) {
		std::string mnemonic_password;
		invariant(get("mnemonic-password", mnemonic_password), "");
		mnemonic                    = cn::Bip32Key::check_bip39_mnemonic(mnemonic);
		cn::Bip32Key master_key     = cn::Bip32Key::create_master_key(mnemonic, mnemonic_password);
		cn::Bip32Key k0             = master_key.derive_key(0x8000002c);
		cn::Bip32Key k1             = k0.derive_key(0x80000300);
		cn::Bip32Key k2             = k1.derive_key(0x80000000 + m_address_type);
		cn::Bip32Key k3             = k2.derive_key(0);
		cn::Bip32Key k4             = k3.derive_key(0);
		m_seed                      = crypto::cn_fast_hash(k4.get_priv_key().data(), k4.get_priv_key().size());
		m_tx_derivation_seed        = derive_from_seed(m_seed, "tx_derivation");
		BinaryArray sk_data         = m_seed | "spend_key_base";
		m_spend_key_base.secret_key = crypto::hash_to_scalar(sk_data.data(), sk_data.size());
		invariant(crypto::secret_key_to_public_key(m_spend_key_base.secret_key, &m_spend_key_base.public_key), "");
	} else {  // View only
		BinaryArray ba;
		invariant(get("spend_key_base_public_key", ba) && ba.size() == sizeof(m_spend_key_base.public_key), "");
		memcpy(m_spend_key_base.public_key.data, ba.data(), ba.size());
		invariant(crypto::key_isvalid(m_spend_key_base.public_key), "Wallet Corrupted - spend key base is invalid");
		if (get("tx_derivation_seed", ba) && ba.size() == sizeof(m_tx_derivation_seed))
			memcpy(m_tx_derivation_seed.data, ba.data(), ba.size());
		// only if we have output_secret_derivation_seed, view-only wallet will be able to see outgoing addresses
	}
	BinaryArray vk_data =
	    BinaryArray{std::begin(m_spend_key_base.public_key.data), std::end(m_spend_key_base.public_key.data)} |
	    "view_key";
	m_view_secret_key = crypto::hash_to_scalar(vk_data.data(), vk_data.size());
	invariant(crypto::secret_key_to_public_key(m_view_secret_key, &m_view_public_key), "");
	{
		BinaryArray ba;
		if (get(ADDRESS_COUNT_PREFIX, ba))
			seria::from_binary(m_used_address_count, ba);
		if (get(CREATION_TIMESTAMP_PREFIX + net_append(m_currency.net), ba))
			seria::from_binary(m_oldest_timestamp, ba);
		else
			m_oldest_timestamp = 0;
	}
	generate_ahead();

	sqlite::Stmt stmt_get;
	stmt_get.prepare(m_db_dbi, "SELECT address, label FROM labels");
	while (stmt_get.step()) {
		auto address_size = stmt_get.column_bytes(0);
		auto address_data = stmt_get.column_blob(0);
		auto label_size   = stmt_get.column_bytes(1);
		auto label_data   = stmt_get.column_blob(1);
		BinaryArray ka    = decrypt_data(m_wallet_key, address_data, address_size);
		BinaryArray ba    = decrypt_data(m_wallet_key, label_data, label_size);

		m_labels[std::string{ka.begin(), ka.end()}] = std::string{ba.begin(), ba.end()};
	}
}

void WalletHD::generate_ahead1(size_t counter, std::vector<WalletRecord> &result) const {
	std::vector<KeyPair> key_result;
	key_result.resize(result.size());
	Hash view_seed;
	memcpy(view_seed.data, m_spend_key_base.public_key.data, sizeof(m_spend_key_base.public_key.data));
	crypto::generate_hd_spendkeys(m_spend_key_base, view_seed, counter, &key_result);
	for (size_t i = 0; i != result.size(); ++i) {
		WalletRecord &record      = result[i];
		record.spend_secret_key   = key_result.at(i).secret_key;
		record.spend_public_key   = key_result.at(i).public_key;
		record.creation_timestamp = std::numeric_limits<Timestamp>::max();  // So adding an address will never rescan
	}
}

void WalletHD::generate_ahead() {
	if (m_wallet_records.size() >= m_used_address_count + GENERATE_AHEAD)
		return;
	size_t delta = m_used_address_count + GENERATE_AHEAD - m_wallet_records.size();
	std::vector<std::vector<WalletRecord>> results;
	if (delta < 1000) {  // TODO - arbitrary constant when single-threaded generation is faster
		results.resize(1);
		results[0].resize(delta);
		generate_ahead1(m_wallet_records.size(), results[0]);
	} else {
		const size_t thc = std::thread::hardware_concurrency();
		results.resize(thc);
		std::vector<std::thread> workers;
		for (size_t i = 0; i != thc; i++) {
			size_t start = delta * i / thc;
			results[i].resize(delta * (i + 1) / thc - start);
			workers.push_back(std::thread(
			    std::bind(&WalletHD::generate_ahead1, this, m_wallet_records.size() + start, std::ref(results[i]))));
		}
		std::for_each(workers.begin(), workers.end(), [](std::thread &t) { t.join(); });
	}
	m_wallet_records.reserve(m_used_address_count + GENERATE_AHEAD);
	for (const auto &result : results)
		for (const auto &record : result) {
			m_records_map.insert(std::make_pair(record.spend_public_key, m_wallet_records.size()));
			m_wallet_records.push_back(record);
		}
}

// std::string WalletHD::hash_for_key(const Hash & key)const{
//	BinaryArray seed_data(std::begin(key.data), std::end(key.data));
//	common::append(seed_data, std::begin(m_view_seed.data), std::end(m_view_seed.data));
//	Hash ha = crypto::cn_fast_hash(seed_data.data(), seed_data.size());
//	return std::string(ha.data, ha.data + sizeof(ha.data));
//}

BinaryArray WalletHD::encrypt_data(const crypto::chacha_key &wallet_key, const BinaryArray &data) {
	const size_t MIN_SIZE   = 256;
	const size_t EXTRA_SIZE = sizeof(Hash) + 4;  // iv, actual size in le
	size_t actual_size      = 1;
	while (actual_size < data.size() + EXTRA_SIZE || actual_size < MIN_SIZE)
		actual_size *= 2;
	BinaryArray large_data(actual_size - sizeof(Hash));
	common::uint_le_to_bytes(large_data.data(), 4, data.size());
	memcpy(large_data.data() + 4, data.data(), data.size());
	BinaryArray enc_data(sizeof(Hash) + large_data.size());
	Hash iv = crypto::rand<Hash>();
	memcpy(enc_data.data(), iv.data, sizeof(iv));
	BinaryArray key_data = BinaryArray{wallet_key.data, wallet_key.data + sizeof(wallet_key.data)} | iv;
	crypto::chacha_key key{crypto::cn_fast_hash(key_data.data(), key_data.size())};
	chacha(20, large_data.data(), large_data.size(), key, crypto::chacha_iv{}, enc_data.data() + sizeof(Hash));
	return enc_data;
}

BinaryArray WalletHD::decrypt_data(const crypto::chacha_key &wallet_key, const uint8_t *value_data, size_t value_size) {
	Hash iv;
	invariant(value_size >= 4 + sizeof(Hash), "");
	memcpy(iv.data, value_data, sizeof(Hash));
	BinaryArray result(value_size - sizeof(Hash));
	BinaryArray key_data = BinaryArray{wallet_key.data, wallet_key.data + sizeof(wallet_key.data)} | iv;
	crypto::chacha_key key{crypto::cn_fast_hash(key_data.data(), key_data.size())};
	chacha(20, value_data + sizeof(Hash), result.size(), key, crypto::chacha_iv{}, result.data());
	auto real_size = common::uint_le_from_bytes<size_t>(result.data(), 4);
	invariant(real_size <= result.size() - 4, "");
	return BinaryArray{result.data() + 4, result.data() + 4 + real_size};
}

void WalletHD::put_salt(const BinaryArray &salt) {
	sqlite::Stmt stmt_update;
	stmt_update.prepare(m_db_dbi, "REPLACE INTO unencrypted (key, value) VALUES ('salt', ?)");
	stmt_update.bind_blob(1, salt.data(), salt.size());
	invariant(!stmt_update.step(), "");
}
BinaryArray WalletHD::get_salt() const {
	sqlite::Stmt stmt_get;
	stmt_get.prepare(m_db_dbi, "SELECT value FROM unencrypted WHERE key = 'salt'");
	invariant(stmt_get.step(), "");
	auto salt_size = stmt_get.column_bytes(0);
	auto salt_data = stmt_get.column_blob(0);
	return BinaryArray{salt_data, salt_data + salt_size};
}

void WalletHD::put(const std::string &key, const BinaryArray &value, bool nooverwrite) {
	Hash key_hash         = derive_from_key(m_wallet_key, "db_parameters" + key);
	BinaryArray enc_key   = encrypt_data(m_wallet_key, BinaryArray{key.data(), key.data() + key.size()});
	BinaryArray enc_value = encrypt_data(m_wallet_key, value);
	sqlite::Stmt stmt_update;
	stmt_update.prepare(m_db_dbi,
	    nooverwrite ? "INSERT INTO parameters (key_hash, key, value) VALUES (?, ?, ?)"
	                : "REPLACE INTO parameters (key_hash, key, value) VALUES (?, ?, ?)");
	stmt_update.bind_blob(1, key_hash.data, sizeof(key_hash.data));
	stmt_update.bind_blob(2, enc_key.data(), enc_key.size());
	stmt_update.bind_blob(3, enc_value.data(), enc_value.size());
	invariant(!stmt_update.step(), "");
}

bool WalletHD::get(const std::string &key, common::BinaryArray &value) const {
	Hash key_hash = derive_from_key(m_wallet_key, "db_parameters" + key);

	sqlite::Stmt stmt_get;
	stmt_get.prepare(m_db_dbi, "SELECT value FROM parameters WHERE key_hash = ?");
	stmt_get.bind_blob(1, key_hash.data, sizeof(key_hash.data));
	if (!stmt_get.step())
		return false;
	auto label_size = stmt_get.column_bytes(0);
	auto label_data = stmt_get.column_blob(0);
	value           = decrypt_data(m_wallet_key, label_data, label_size);
	return true;
}

void WalletHD::put(const std::string &key, const std::string &value, bool nooverwrite) {
	return put(key, common::as_binary_array(value), nooverwrite);
}
bool WalletHD::get(const std::string &key, std::string &value) const {
	common::BinaryArray ba;
	if (!get(key, ba))
		return false;
	value = common::as_string(ba);
	return true;
}

std::vector<WalletRecord> WalletHD::generate_new_addresses(
    const std::vector<SecretKey> &sks, Timestamp ct, Timestamp now, bool *rescan_from_ct) {
	for (const auto &sk : sks)
		if (sk != SecretKey{})
			throw std::runtime_error("Generating non-deterministic addreses not supported by HD wallet");
	std::vector<WalletRecord> result;
	if (sks.empty())
		return result;
	auto was_used_address_count = m_used_address_count;
	m_used_address_count += sks.size();
	generate_ahead();
	for (size_t i = 0; i != sks.size(); ++i)
		result.push_back(m_wallet_records.at(was_used_address_count + i));
	put(ADDRESS_COUNT_PREFIX, seria::to_binary(m_used_address_count), false);
	commit();
	return result;
}

AccountAddress WalletHD::record_to_address(const WalletRecord &record) const {
	PublicKey sv = generate_address_s_v(record.spend_public_key, m_view_secret_key);
	// TODO - do multiplication only once
	return AccountAddressUnlinkable{
	    record.spend_public_key, sv, m_address_type == AccountAddressUnlinkable::type_tag_auditable};
}

bool WalletHD::get_record(WalletRecord *record, const AccountAddress &v_addr) const {
	if (v_addr.type() != typeid(AccountAddressUnlinkable))
		return false;
	auto &addr = boost::get<AccountAddressUnlinkable>(v_addr);
	if (addr.is_auditable != is_auditable())
		return false;
	auto rit = m_records_map.find(addr.s);
	if (rit == m_records_map.end() || rit->second >= get_actual_records_count())
		return false;
	// TODO - do not call record_to_address
	auto addr2 = record_to_address(m_wallet_records.at(rit->second));
	if (v_addr != addr2)
		return false;
	//	invariant (m_wallet_records.at(rit->second).spend_public_key == addr.spend_public_key, "");
	*record = m_wallet_records.at(rit->second);
	return true;
}

void WalletHD::set_password(const std::string &password) {
	auto parameters = parameters_get();
	auto pq2        = payment_queue_get2();

	m_db_dbi.exec("DELETE FROM payment_queue");
	m_db_dbi.exec("DELETE FROM parameters");
	m_db_dbi.exec("DELETE FROM labels");

	BinaryArray salt(sizeof(Hash));
	crypto::generate_random_bytes(salt.data(), salt.size());
	put_salt(salt);
	common::append(salt, BinaryArray{password.data(), password.data() + password.size()});
	crypto::CryptoNightContext cn_ctx;
	m_wallet_key = generate_chacha8_key(cn_ctx, salt.data(), salt.size());

	for (const auto &p : parameters)
		put(p.first, p.second, true);
	for (const auto &l : m_labels)
		set_label(l.first, l.second);
	for (const auto &el : pq2)
		payment_queue_add(std::get<0>(el), std::get<1>(el), std::get<2>(el));
	commit();
}

void WalletHD::export_wallet(const std::string &export_path, const std::string &new_password, bool view_only,
    bool view_outgoing_addresses) const {
	WalletHD other(m_currency, m_log.get_logger(), export_path, new_password, std::string(), 0, 0, std::string());

	if (!is_view_only() && view_only) {
		other.put("spend_key_base_public_key",
		    BinaryArray{std::begin(m_spend_key_base.public_key.data), std::end(m_spend_key_base.public_key.data)},
		    true);
		if (view_outgoing_addresses)
			other.put("tx_derivation_seed",
			    BinaryArray{std::begin(m_tx_derivation_seed.data), std::end(m_tx_derivation_seed.data)}, true);
		for (const auto &p : parameters_get())
			if (p.first != "mnemonic" && p.first != "mnemonic-password")
				other.put(p.first, p.second, true);
		for (const auto &l : m_labels)
			other.set_label(l.first, l.second);
	} else {
		for (const auto &p : parameters_get())
			other.put(p.first, p.second, true);
		for (const auto &l : m_labels)
			other.set_label(l.first, l.second);
		for (const auto &el : payment_queue_get2())
			other.payment_queue_add(std::get<0>(el), std::get<1>(el), std::get<2>(el));
	}
	other.commit();
}

std::string WalletHD::export_keys() const {
	std::string mnemonic;
	if (!get("mnemonic", mnemonic))
		throw std::runtime_error("Exporting keys (mnemonic) not supported by view-only HD wallet");
	return mnemonic;
}

void WalletHD::on_first_output_found(Timestamp ts) {
	BinaryArray ba;
	if (m_oldest_timestamp != 0 || ts == 0)
		return;
	put(CREATION_TIMESTAMP_PREFIX + net_append(m_currency.net), seria::to_binary(ts), false);
	commit();
}

void WalletHD::create_look_ahead_records(size_t count) {
	if (count <= m_used_address_count)
		return;
	m_used_address_count = count;
	generate_ahead();
	put(ADDRESS_COUNT_PREFIX, seria::to_binary(m_used_address_count), false);
	commit();
}

void WalletHD::backup(const std::string &dst_name, const std::string &pass) const {
	export_wallet(dst_name, pass, false, false);
}

Wallet::History WalletHD::load_history(const Hash &tid) const { return Wallet::History{}; }

std::vector<BinaryArray> WalletHD::payment_queue_get() const {
	std::vector<BinaryArray> result;
	auto pq2 = payment_queue_get2();
	for (const auto &el : pq2)
		if (std::get<1>(el) == m_currency.net)
			result.push_back(std::get<2>(el));
	return result;
}

std::vector<std::tuple<Hash, std::string, BinaryArray>> WalletHD::payment_queue_get2() const {
	std::vector<std::tuple<Hash, std::string, BinaryArray>> result;
	sqlite::Stmt stmt_get;
	stmt_get.prepare(m_db_dbi, "SELECT tid, net, binary_transaction FROM payment_queue");
	while (stmt_get.step()) {
		auto tid_size = stmt_get.column_bytes(0);
		auto tid_data = stmt_get.column_blob(0);
		auto net_size = stmt_get.column_bytes(1);
		auto net_data = stmt_get.column_blob(1);
		auto btx_size = stmt_get.column_bytes(2);
		auto btx_data = stmt_get.column_blob(2);

		BinaryArray key = decrypt_data(m_wallet_key, tid_data, tid_size);
		invariant(key.size() == sizeof(Hash), "");
		Hash tid;
		memcpy(tid.data, key.data(), sizeof(Hash));
		BinaryArray net = decrypt_data(m_wallet_key, net_data, net_size);
		BinaryArray ba  = decrypt_data(m_wallet_key, btx_data, btx_size);
		result.push_back(std::make_tuple(tid, std::string(net.begin(), net.end()), ba));
	}
	return result;
}

void WalletHD::payment_queue_add(const Hash &tid, const std::string &net, const BinaryArray &binary_transaction) {
	Hash tid_hash =
	    derive_from_key(m_wallet_key, "db_payment_queue_tid" + std::string(std::begin(tid.data), std::end(tid.data)));
	Hash net_hash         = derive_from_key(m_wallet_key, "db_payment_queue_net" + net);
	BinaryArray enc_tid   = encrypt_data(m_wallet_key, BinaryArray{tid.data, tid.data + sizeof(tid.data)});
	BinaryArray enc_net   = encrypt_data(m_wallet_key, BinaryArray{net.data(), net.data() + net.size()});
	BinaryArray enc_value = encrypt_data(m_wallet_key, binary_transaction);
	sqlite::Stmt stmt_update;
	stmt_update.prepare(m_db_dbi,
	    "REPLACE INTO payment_queue (tid_hash, net_hash, tid, net, binary_transaction) VALUES (?, ?, ?, ?, ?)");
	stmt_update.bind_blob(1, tid_hash.data, sizeof(tid_hash.data));
	stmt_update.bind_blob(2, net_hash.data, sizeof(net_hash.data));
	stmt_update.bind_blob(3, enc_tid.data(), enc_tid.size());
	stmt_update.bind_blob(4, enc_net.data(), enc_net.size());
	stmt_update.bind_blob(5, enc_value.data(), enc_value.size());
	invariant(!stmt_update.step(), "");
}

std::vector<std::pair<std::string, BinaryArray>> WalletHD::parameters_get() const {
	std::vector<std::pair<std::string, BinaryArray>> result;
	sqlite::Stmt stmt_get;
	stmt_get.prepare(m_db_dbi, "SELECT key, value FROM parameters");
	while (stmt_get.step()) {
		auto key_size = stmt_get.column_bytes(0);
		;
		auto key_data   = stmt_get.column_blob(0);
		auto value_size = stmt_get.column_bytes(1);
		;
		auto value_data = stmt_get.column_blob(1);

		BinaryArray ka  = decrypt_data(m_wallet_key, key_data, key_size);
		BinaryArray ba  = decrypt_data(m_wallet_key, value_data, value_size);
		std::string key = std::string(ka.begin(), ka.end());
		result.push_back(std::make_pair(key, ba));
	}
	return result;
}

void WalletHD::payment_queue_add(const Hash &tid, const BinaryArray &binary_transaction) {
	payment_queue_add(tid, m_currency.net, binary_transaction);
}

void WalletHD::commit() {
	m_db_dbi.commit_txn();
	m_db_dbi.begin_txn();
}

void WalletHD::payment_queue_remove(const Hash &tid) {
	Hash tid_hash =
	    derive_from_key(m_wallet_key, "db_payment_queue_tid" + std::string(std::begin(tid.data), std::end(tid.data)));
	Hash net_hash = derive_from_key(m_wallet_key, "db_payment_queue_net" + m_currency.net);

	sqlite::Stmt stmt_del;
	stmt_del.prepare(m_db_dbi, "DELETE FROM payment_queue WHERE net_hash = ? AND tid_hash = ?");
	stmt_del.bind_blob(1, net_hash.data, sizeof(net_hash.data));
	stmt_del.bind_blob(2, tid_hash.data, sizeof(tid_hash.data));
	invariant(!stmt_del.step(), "");

	if (tid.data[0] == 'x')  // committing here is not so critical, improve speed here
		commit();
}

void WalletHD::set_label(const std::string &address, const std::string &label) {
	Hash address_hash       = derive_from_key(m_wallet_key, "db_labels" + address);
	BinaryArray enc_address = encrypt_data(m_wallet_key, BinaryArray{address.data(), address.data() + address.size()});
	BinaryArray enc_label   = encrypt_data(m_wallet_key, BinaryArray{label.data(), label.data() + label.size()});

	if (label.empty()) {
		m_labels.erase(address);
		sqlite::Stmt stmt_del;
		stmt_del.prepare(m_db_dbi, "DELETE FROM labels WHERE address_hash = ?");
		stmt_del.bind_blob(1, address_hash.data, sizeof(address_hash.data));
		invariant(!stmt_del.step(), "");
	} else {
		m_labels[address] = label;

		sqlite::Stmt stmt_update;
		stmt_update.prepare(m_db_dbi, "REPLACE INTO labels (address_hash, address, label) VALUES (?, ?, ?)");
		stmt_update.bind_blob(1, address_hash.data, sizeof(address_hash.data));
		stmt_update.bind_blob(2, enc_address.data(), enc_address.size());
		stmt_update.bind_blob(3, enc_label.data(), enc_label.size());
		invariant(!stmt_update.step(), "");
	}
	commit();
}

std::string WalletHD::get_label(const std::string &address) const {
	auto lit = m_labels.find(address);
	if (lit == m_labels.end())
		return std::string();
	return lit->second;
}

Wallet::OutputHandler WalletHD::get_output_handler() const {
	SecretKey vsk_copy = m_view_secret_key;
	return
	    [vsk_copy](const PublicKey &tx_public_key, boost::optional<KeyDerivation> *kd, const Hash &tx_inputs_hash,
	        size_t output_index, const OutputKey &key_output, PublicKey *spend_public_key, SecretKey *secret_scalar) {
		    *spend_public_key = crypto::unlinkable_underive_public_key(vsk_copy, tx_inputs_hash, output_index,
		        key_output.public_key, key_output.encrypted_secret, secret_scalar);
	    };
}

bool WalletHD::detect_our_output(const Hash &tid, const Hash &tx_inputs_hash, const boost::optional<KeyDerivation> &kd,
    size_t out_index, const PublicKey &spend_public_key, const SecretKey &secret_scalar, const OutputKey &key_output,
    Amount *amount, KeyPair *output_keypair, AccountAddress *address) {
	WalletRecord record;
	if (!get_look_ahead_record(record, spend_public_key))
		return false;
	AccountAddress addr   = record_to_address(record);
	const auto &u_address = boost::get<AccountAddressUnlinkable>(addr);  // TODO - better logic
	if (u_address.is_auditable != key_output.is_auditable)
		return false;
	if (record.spend_secret_key != SecretKey{}) {
		output_keypair->secret_key = crypto::unlinkable_derive_secret_key(record.spend_secret_key, secret_scalar);
		if (!crypto::secret_key_to_public_key(output_keypair->secret_key, &output_keypair->public_key) ||
		    output_keypair->public_key != key_output.public_key)
			return false;
	}
	*address = addr;
	//							std::cout << "My unlinkable output! out_index=" << out_index <<
	// "amount=" << key_output.amount << std::endl;
	*amount = key_output.amount;
	return true;
}
