#include "SecureKV.h"

SecureKV::SecureKV() {
	if (!std::filesystem::exists(Config::getConfigPath()))
		std::filesystem::create_directory(Config::getConfigPath());
	openFile();
}

SecureKV::~SecureKV() {
	m_file.close();
}

void SecureKV::openFile() {
	if (!std::filesystem::exists(Config::getConfigPath() + SECUREKV_FILENAME)) {
		//create an empty file so that openFile doesn't fail (due to the std::ios::in flag)
		std::ofstream(Config::getConfigPath() + SECUREKV_FILENAME).close();
	}
	m_file.open(Config::getConfigPath() + SECUREKV_FILENAME,
		std::ios::in | std::ios::out | std::ios::binary,
		_SH_DENYRW
	);
	if (!m_file.is_open())
		throw std::runtime_error("Failed to open secure file!");
}

void SecureKV::reopenFile(bool remove) {
	if (m_file.is_open()) m_file.close();
	if (remove) {
		try {
			std::filesystem::remove(Config::getConfigPath() + SECUREKV_FILENAME);
		}
		catch (std::exception& e) {
			g_logger.warning(sf() << "Failed to remove secure file :" << e.what());
		}
	}
	openFile();
}

void SecureKV::write(const secure_string& key, const secure_string& value, const KeyData& keydata) {
	const std::lock_guard<std::mutex> lock(m_high_mutex);
	KVs kvs = load(keydata);
	auto i = std::find_if(kvs.begin(), kvs.end(), [&key](const KV& kv) {return kv.first == secure_string(key); });
	if (i == kvs.end()) {//the key doesn't exist yet
		kvs.push_back({key, value});
	}
	else {//key already exists
		i->second = value;
	}

	save(kvs, keydata);
}

secure_string SecureKV::read(const secure_string& key, const KeyData& keydata) {
	const std::lock_guard<std::mutex> lock(m_high_mutex);
	KVs kvs = load(keydata);
	auto i = std::find_if(kvs.begin(), kvs.end(), [&key](const KV& kv) {
		return kv.first == secure_string(key);
	});
	if (i == kvs.end()) {//the key doesn't exist
		return secure_string();
	}

	return i->second;
}

bool SecureKV::save(const KVs& content, const KeyData& keydata) {
	const std::lock_guard<std::mutex> lock(m_low_mutex);
	reopenFile(true);

	try {
		secure_string dump;
		for (const auto& [k, v] : content) {
			//Sanity check
			if (k.find(SECUREKV_DELIM) != secure_string::npos ||
				v.find(SECUREKV_DELIM) != secure_string::npos)
				throw std::runtime_error(sf() << "invalid data at k=" << k);//k should not contain anything sensitive.

			//Dump
			dump += k + SECUREKV_DELIM + v + SECUREKV_DELIM;
		}

		if (keydata.type == EncryptionType::HWID)
			dump = Crypto::encryptHWID(dump);
		else if (keydata.type == EncryptionType::Password)
			dump = Crypto::encrypt(dump, keydata.key, keydata.iv);
		else if (keydata.type == EncryptionType::HWIDAndPassword)
			dump = Crypto::encrypt(Crypto::encryptHWID(dump), keydata.key, keydata.iv);
		else
			throw std::runtime_error("unknown encryption type");

		dump.insert(0, SECUREKV_HEADER);
		if (keydata.type == EncryptionType::HWID)
			dump.insert(3, "\001");
		else if (keydata.type == EncryptionType::Password)
			dump.insert(3, "\002");
		else if (keydata.type == EncryptionType::HWIDAndPassword)
			dump.insert(3, "\003");

		m_file.write(dump.data(), dump.size());
		m_file << std::flush;
	}
	catch (std::exception& e) {
		g_logger.error(sf() << "Failed to save secure : " << e.what());
		return false;
	}

	return true;
}

SecureKV::KVs SecureKV::load(const KeyData& keydata) {
	const std::lock_guard<std::mutex> lock(m_low_mutex);

	KVs out;

	m_file.seekg(0);
	secure_string file_str((std::istreambuf_iterator<char>(m_file)), std::istreambuf_iterator<char>());
	
	try {
		if (file_str.empty())
			throw std::runtime_error("file is empty");
		if (file_str.size() <= 4)//More?
			throw std::runtime_error("file is too short");

		//TODO change?
		if (file_str[0] != SECUREKV_HEADER[0] ||
			file_str[1] != SECUREKV_HEADER[1] ||
			file_str[2] != SECUREKV_HEADER[2])
			throw std::runtime_error("invalid header");

		int encryptionType = file_str[3];
		file_str.erase(0, 4);

		if (encryptionType == 0x01) {
			if (keydata.type != EncryptionType::HWID)
				throw std::runtime_error("encryption type mismatch 0x01");
			file_str = Crypto::decryptHWID(file_str);
		}
		else if (encryptionType == 0x02) {
			if (keydata.type != EncryptionType::Password)
				throw std::runtime_error("encryption type mismatch 0x02");
			file_str = Crypto::decrypt(file_str, keydata.key, keydata.iv);
		}
		else if (encryptionType == 0x03) {
			if (keydata.type != EncryptionType::HWIDAndPassword)
				throw std::runtime_error("encryption type mismatch 0x03");
			file_str = Crypto::decryptHWID(Crypto::decrypt(file_str, keydata.key, keydata.iv));
		}
		else {
			throw std::runtime_error("unknown encryption type");
		}

		//Parse!
		secure_string line;
		size_t delim_pos = file_str.find(SECUREKV_DELIM);

		while (delim_pos != secure_string::npos) {
			secure_string key, value;
			key = file_str.substr(0, delim_pos);
			file_str.erase(0, delim_pos + 1);

			delim_pos = file_str.find(SECUREKV_DELIM);
			if (delim_pos == secure_string::npos)
				throw std::runtime_error("corrumpted data");

			value = file_str.substr(0, delim_pos);
			file_str.erase(0, delim_pos + 1);

			out.push_back({ key, value });

			delim_pos = file_str.find(SECUREKV_DELIM);
		}
	}
	catch (std::exception& e) {
		g_logger.error(sf() << "Failed to load secure : " << e.what());
		return {};
	}

	return out;
}

void SecureKV::reencrypt(const KeyData& oldKeydata, const KeyData& newKeydata) {
	const std::lock_guard<std::mutex> lock(m_high_mutex);
	KVs content = load(oldKeydata);
	if (content.empty()) {
		g_logger.error("Failed to reencrypt : failed to load");
		return;
	}
	save(content, newKeydata);
}

EncryptionType SecureKV::getEncryptionType() {
	const std::lock_guard<std::mutex> lock(m_low_mutex);

	m_file.seekg(3);
	if (!m_file.good()) {
		g_logger.warning("Unable to get the secure encryption type : the file is too short.");
		return EncryptionType::Unknown;
	}

	char encryptionType = 0x00;
	m_file.read(&encryptionType, 1);

	if (encryptionType == 0x01) return EncryptionType::HWID;
	if (encryptionType == 0x02) return EncryptionType::Password;
	if (encryptionType == 0x03) return EncryptionType::HWIDAndPassword;
	
	return EncryptionType::Unknown;
}