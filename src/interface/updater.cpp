#include "filezilla.h"

#if FZ_MANUALUPDATECHECK

#include "buildinfo.h"
#include "file_utils.h"
#include "Options.h"
#include "updater.h"
#include "../commonui/updater_cert.h"
#include "serverdata.h"
#include <string>

#ifdef __WXMSW__
#include <wx/msw/registry.h>
#endif

#include "../include/version.h"
#include "../include/writer.h"

#include <libfilezilla/file.hpp>
#include <libfilezilla/hash.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/signature.hpp>
#include <libfilezilla/translate.hpp>
#include <libfilezilla/glue/wxinvoker.hpp>

#include <string>

BEGIN_EVENT_TABLE(CUpdater, wxEvtHandler)
EVT_TIMER(wxID_ANY, CUpdater::OnTimer)
END_EVENT_TABLE()

void version_information::update_available()
{
	if (!nightly_.url_.empty() && COptions::Get()->get_int(OPTION_UPDATECHECK_CHECKBETA) == 2) {
		available_ = nightly_;
	}
	else if (!beta_.version_.empty() && COptions::Get()->get_int(OPTION_UPDATECHECK_CHECKBETA) != 0) {
		available_ = beta_;
	}
	else if (!stable_.version_.empty()) {
		available_ = stable_;
	}
	else {
		available_ = build();
	}
}

static CUpdater* instance = 0;

CUpdater::CUpdater(CUpdateHandler& parent, CFileZillaEngineContext& engine_context)
	: state_(UpdaterState::idle)
	, engine_context_(engine_context)
{
	AddHandler(parent);
}

void CUpdater::Init()
{
	if (Busy()) {
		return;
	}

	if (COptions::Get()->get_int(OPTION_DEFAULT_DISABLEUPDATECHECK) != 0 || !LongTimeSinceLastCheck()) {
		raw_version_information_ = COptions::Get()->get_string(OPTION_UPDATECHECK_NEWVERSION);
	}

	UpdaterState s = ProcessFinishedData(FZ_AUTOUPDATECHECK);

	SetState(s);

	AutoRunIfNeeded();

	update_timer_.SetOwner(this);
	update_timer_.Start(1000 * 3600);

	if (!instance) {
		instance = this;
	}
}

CUpdater::~CUpdater()
{
	if (instance == this) {
		instance = 0;
	
	}

	delete engine_;
}

CUpdater* CUpdater::GetInstance()
{
	return instance;
}

void CUpdater::AutoRunIfNeeded()
{
#if FZ_AUTOUPDATECHECK
	if (state_ == UpdaterState::failed || state_ == UpdaterState::idle || state_ == UpdaterState::newversion_stale) {
		if (!COptions::Get()->get_int(OPTION_DEFAULT_DISABLEUPDATECHECK) && COptions::Get()->get_int(OPTION_UPDATECHECK) != 0) {
			if (LongTimeSinceLastCheck()) {
				Run(false);
			}
		}
		else {
			auto const age = fz::datetime::now() - CBuildInfo::GetBuildDate();
			if (age >= fz::duration::from_days(31*6)) {
				version_information_ = version_information();
				SetState(UpdaterState::newversion_stale);
			}
		}
	}
#endif
}

void CUpdater::RunIfNeeded()
{
	build const b = AvailableBuild();
	if (state_ == UpdaterState::idle || state_ == UpdaterState::failed || state_ == UpdaterState::newversion_stale ||
		state_ == UpdaterState::eol ||
		LongTimeSinceLastCheck() || (state_ == UpdaterState::newversion && !b.url_.empty()) ||
		(state_ == UpdaterState::newversion_ready && !VerifyChecksum(DownloadedFile(), b.size_, b.hash_)))
	{
		Run(true);
	}
}

bool CUpdater::LongTimeSinceLastCheck() const
{
	std::wstring const lastCheckStr = COptions::Get()->get_string(OPTION_UPDATECHECK_LASTDATE);
	if (lastCheckStr.empty()) {
		return true;
	}

	fz::datetime lastCheck(lastCheckStr, fz::datetime::utc);
	if (lastCheck.empty()) {
		return true;
	}

	auto const span = fz::datetime::now() - lastCheck;

	if (span.get_seconds() < 0) {
		// Last check in future
		return true;
	}

	int days = 1;
	if (!CBuildInfo::IsUnstable()) {
		days = COptions::Get()->get_int(OPTION_UPDATECHECK_INTERVAL);
	}
	return span.get_days() >= days;
}

fz::uri CUpdater::GetUrl()
{
	fz::uri uri("https://update.filezilla-project.org/update.php");
	fz::query_string qs;
	
	std::string host = fz::to_utf8(CBuildInfo::GetHostname());
	if (host.empty()) {
		host = "unknown";
	}
	qs["platform"] = host;
	qs["version"] = fz::to_utf8(GetFileZillaVersion());

#if defined(__WXMSW__) || defined(__WXMAC__)
	// Makes not much sense to submit OS version on Linux, *BSD and the likes, too many flavours.
	qs["osversion"] = fz::sprintf("%d.%d", wxPlatformInfo::Get().GetOSMajorVersion(), wxPlatformInfo::Get().GetOSMinorVersion());
#endif

#ifdef __WXMSW__
	if (wxIsPlatform64Bit()) {
		qs["osarch"] = "64";
	}
	else {
		qs["osarch"] = "32";
	}

	// Add information about package
	{
		wxLogNull log;

		// Installer always writes to 32bit section
		auto key = std::make_unique<wxRegKey>(_T("HKEY_CURRENT_USER\\Software\\FileZilla Client"), wxRegKey::WOW64ViewMode_32);
		if (!key->Exists()) {
			// wxRegKey is sad, it doesn't even have a copy constructor.
			key = std::make_unique<wxRegKey>(_T("HKEY_LOCAL_MACHINE\\Software\\FileZilla Client"), wxRegKey::WOW64ViewMode_32);
		}

		long updated{};
		if (key->GetValueType(_T("Updated")) == wxRegKey::Type_Dword && key->QueryValue(_T("Updated"), &updated)) {
			qs["updated"] = fz::to_string(updated);
		}

		long package{};
		if (key->GetValueType(_T("Package")) == wxRegKey::Type_Dword && key->QueryValue(_T("Package"), &package)) {
			qs["package"] = fz::to_string(package);
		}

		wxString channel;
		if (key->GetValueType(_T("Channel")) == wxRegKey::Type_String && key->QueryValue(_T("Channel"), channel)) {
			qs["channel"] = fz::to_utf8(channel);
		}
	}
#endif

	std::string const cpuCaps = fz::to_utf8(CBuildInfo::GetCPUCaps(','));
	if (!cpuCaps.empty()) {
		qs["cpuid"] = cpuCaps;
	}

	std::wstring const lastVersion = COptions::Get()->get_string(OPTION_UPDATECHECK_LASTVERSION);
	if (lastVersion != GetFileZillaVersion()) {
		qs["initial"] = "1";
	}
	else {
		qs["initial"] = "0";
	}

	if (manual_) {
		qs["manual"] = "1";
	}

	if (GetEnv("FZUPDATETEST") == L"1") {
		qs["test"] = "1";
	}
	uri.query_ = qs.to_string(true);
	return uri;
}

bool CUpdater::Run(bool manual)
{
	if (state_ != UpdaterState::idle && state_ != UpdaterState::failed &&
		state_ != UpdaterState::newversion && state_ != UpdaterState::newversion_ready
		&& state_ != UpdaterState::newversion_stale && state_ != UpdaterState::eol)
	{
		return false;
	}

	auto const t = fz::datetime::now();
	COptions::Get()->set(OPTION_UPDATECHECK_LASTDATE, t.format(L"%Y-%m-%d %H:%M:%S", fz::datetime::utc));

	local_file_.clear();
	log_ = fz::sprintf(_("Started update check on %s\n"), t.format(L"%Y-%m-%d %H:%M:%S", fz::datetime::local));
	manual_ = manual;

	std::wstring build = CBuildInfo::GetBuildType();
	if (build.empty())  {
		build = _("custom").ToStdWstring();
	}
	log_ += fz::sprintf(_("Own build type: %s\n"), build);

	SetState(UpdaterState::checking);

	m_use_internal_rootcert = true;
	int res = Request(GetUrl());

	if (res != FZ_REPLY_WOULDBLOCK) {
		SetState(UpdaterState::failed);
	}
	raw_version_information_.clear();

	return state_ == UpdaterState::checking;
}

int CUpdater::Download(std::wstring const& url, std::wstring const& local_file)
{
	wxASSERT(pending_commands_.empty());
	pending_commands_.clear();
	pending_commands_.emplace_back(new CDisconnectCommand);
	if (!CreateConnectCommand(url) || !CreateTransferCommand(url, local_file)) {
		return FZ_REPLY_ERROR;
	}

	return ContinueDownload();
}

int CUpdater::Request(fz::uri const& uri)
{
	wxASSERT(pending_commands_.empty());
	pending_commands_.clear();
	pending_commands_.emplace_back(new CDisconnectCommand);

	CServer server(fz::equal_insensitive_ascii(uri.scheme_, std::string("http")) ? HTTP : HTTPS, DEFAULT, fz::to_wstring_from_utf8(uri.host_), uri.port_);
	pending_commands_.emplace_back(new CConnectCommand(server, ServerHandle(), Credentials()));
	pending_commands_.emplace_back(new CHttpRequestCommand(uri, writer_factory_holder(std::make_unique<memory_writer_factory>(L"Updater", output_buffer_, 1024*1024))));

	return ContinueDownload();
}

int CUpdater::ContinueDownload()
{
	if (pending_commands_.empty()) {
		return FZ_REPLY_OK;
	}

	if (!engine_) {
		engine_ = new CFileZillaEngine(engine_context_, fz::make_invoker(*this, [this](CFileZillaEngine* engine){ OnEngineEvent(engine); }));
	}

	int res = engine_->Execute(*pending_commands_.front());
	if (res == FZ_REPLY_OK) {
		pending_commands_.pop_front();
		return ContinueDownload();
	}

	return res;
}

bool CUpdater::CreateConnectCommand(std::wstring const& url)
{
	Site s;
	CServerPath path;
	std::wstring error;
	if (!s.ParseUrl(url, 0, std::wstring(), std::wstring(), error, path) || (s.server.GetProtocol() != HTTP && s.server.GetProtocol() != HTTPS)) {
		return false;
	}

	pending_commands_.emplace_back(new CConnectCommand(s.server, s.Handle(), s.credentials));
	return true;
}

bool CUpdater::CreateTransferCommand(std::wstring const& url, std::wstring const& local_file)
{
	
	Site s;
	CServerPath path;
	std::wstring error;
	if (!s.ParseUrl(url, 0, std::wstring(), std::wstring(), error, path) || (s.server.GetProtocol() != HTTP && s.server.GetProtocol() != HTTPS)) {
		return false;
	}
	std::wstring file = path.GetLastSegment();
	path = path.GetParent();

	transfer_flags const flags = transfer_flags::download;
	auto cmd = new CFileTransferCommand(file_writer_factory(local_file, true), path, file, flags);
	pending_commands_.emplace_back(cmd);
	return true;
}

void CUpdater::OnEngineEvent(CFileZillaEngine* engine)
{
	if (!engine_ || engine_ != engine) {
		return;
	}

	std::unique_ptr<CNotification> notification;
	while ((notification = engine_->GetNextNotification())) {
		ProcessNotification(std::move(notification));
	}
}

void CUpdater::ProcessNotification(std::unique_ptr<CNotification> && notification)
{
	if (state_ != UpdaterState::checking && state_ != UpdaterState::newversion_downloading) {
		return;
	}

	switch (notification->GetID())
	{
	case nId_asyncrequest:
		{
			auto pData = unique_static_cast<CAsyncRequestNotification>(std::move(notification));
			if (pData->GetRequestID() == reqId_fileexists) {
				static_cast<CFileExistsNotification *>(pData.get())->overwriteAction = CFileExistsNotification::resume;
			}
			else if (pData->GetRequestID() == reqId_certificate) {
				auto & certNotification = static_cast<CCertificateNotification &>(*pData.get());
				if (m_use_internal_rootcert) {
					auto certs = certNotification.info_.get_certificates();
					if (certs.size() > 1) {
						auto const& ca = certs.back();
						std::vector<uint8_t> ca_data = ca.get_raw_data();

						auto const updater_root = fz::base64_decode(updater_cert);
						if (ca_data == updater_root) {
							certNotification.trusted_ = true;
						}
					}
				}
				else {
					certNotification.trusted_ = true;
				}
			}
			engine_->SetAsyncRequestReply(std::move(pData));
		}
		break;
	case nId_operation:
		ProcessOperation(static_cast<COperationNotification const&>(*notification.get()));
		break;
	case nId_logmsg:
		{
			auto const& msg = static_cast<CLogmsgNotification const&>(*notification.get());
			log_ += msg.msg + L"\n";
		}
		break;
	default:
		break;
	}
}

UpdaterState CUpdater::ProcessFinishedData(bool can_download)
{
	UpdaterState s = UpdaterState::failed;

	ParseData();

	if (version_information_.eol_) {
		s = UpdaterState::eol;
	}
	else if (version_information_.available_.version_.empty()) {
		s = UpdaterState::idle;
	}
	else if (!version_information_.available_.url_.empty()) {

		std::wstring const temp = GetTempFile();
		std::wstring const local_file = GetLocalFile(version_information_.available_, true);
		if (!local_file.empty() && fz::local_filesys::get_file_type(fz::to_native(local_file)) != fz::local_filesys::unknown) {
			local_file_ = local_file;
			log_ += fz::sprintf(_("Local file is %s\n"), local_file);
			s = UpdaterState::newversion_ready;
		}
		else {
			// We got a checksum over a secure channel already.
			m_use_internal_rootcert = false;

			if (temp.empty() || local_file.empty()) {
				s = UpdaterState::newversion;
			}
			else {
				s = UpdaterState::newversion_downloading;
				auto size = fz::local_filesys::get_size(fz::to_native(temp));
				if (size >= 0 && size >= version_information_.available_.size_) {
					s = ProcessFinishedDownload();
				}
				else if (!can_download || Download(version_information_.available_.url_, temp) != FZ_REPLY_WOULDBLOCK) {
					s = UpdaterState::newversion;
				}
			}
		}
	}
	else {
		s = UpdaterState::newversion;
	}

	return s;
}

void CUpdater::ProcessOperation(COperationNotification const& operation)
{
	if (state_ != UpdaterState::checking && state_ != UpdaterState::newversion_downloading) {
		return;
	}

	if (pending_commands_.empty()) {
		SetState(UpdaterState::failed);
		return;
	}


	UpdaterState s = UpdaterState::failed;

	int res = operation.replyCode_;
	if (res == FZ_REPLY_OK || (operation.commandId_ == Command::disconnect && res & FZ_REPLY_DISCONNECTED)) {
		pending_commands_.pop_front();
		res = ContinueDownload();
		if (res == FZ_REPLY_WOULDBLOCK) {
			return;
		}
	}

	if (res != FZ_REPLY_OK) {
		if (state_ != UpdaterState::checking) {
			s = UpdaterState::newversion;
		}
	}
	else if (state_ == UpdaterState::checking) {
		if (!FilterOutput()) {
			SetState(UpdaterState::failed);
			return;
		}
		COptions::Get()->set(OPTION_UPDATECHECK_LASTVERSION, GetFileZillaVersion());
		s = ProcessFinishedData(true);
	}
	else {
		s = ProcessFinishedDownload();
	}
	SetState(s);
}

UpdaterState CUpdater::ProcessFinishedDownload()
{
	UpdaterState s = UpdaterState::newversion;

	std::wstring const temp = GetTempFile();
	if (temp.empty()) {
		s = UpdaterState::newversion;
	}
	else if (!VerifyChecksum(temp, version_information_.available_.size_, version_information_.available_.hash_)) {
		fz::remove_file(fz::to_native(temp));
		s = UpdaterState::newversion;
	}
	else {
		s = UpdaterState::newversion_ready;

		std::wstring local_file = GetLocalFile(version_information_.available_, false);

		wxLogNull log;
		if (local_file.empty() || !wxRenameFile(temp, local_file, false)) {
			s = UpdaterState::newversion;
			fz::remove_file(fz::to_native(temp));
			log_ += fz::sprintf(_("Could not create local file %s\n"), local_file);
		}
		else {
			local_file_ = local_file;
			log_ += fz::sprintf(_("Local file is %s\n"), local_file);
		}
	}
	return s;
}

std::wstring CUpdater::GetLocalFile(build const& b, bool allow_existing)
{
	std::wstring const fn = GetFilename(b.url_);
	std::wstring const dl = GetDownloadDir().GetPath();

	int i = 1;
	std::wstring f = dl + fn;

	while (fz::local_filesys::get_file_type(fz::to_native(f)) != fz::local_filesys::unknown && (!allow_existing || !VerifyChecksum(f, b.size_, b.hash_))) {
		if (++i > 99) {
			return std::wstring();
		}

		size_t pos;
		if (fn.size() > 8 && fz::str_tolower_ascii(fn.substr(fn.size() - 8)) == L".tar.bz2") {
			pos = fn.size() - 8;
		}
		else {
			pos = fn.rfind('.');
		}

		if (pos == std::wstring::npos) {
			f = dl + fn + fz::sprintf(L" (%d)", i);
		}
		else {
			f = dl + fn.substr(0, pos) + fz::sprintf(L" (%d)", i) + fn.substr(pos);
		}
	}

	return f;
}

bool CUpdater::FilterOutput()
{
	if (state_ != UpdaterState::checking) {
		return false;
	}

	if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
		log_ += fz::sprintf(L"FilterOutput %u\n", output_buffer_.size());
	}

	raw_version_information_.resize(output_buffer_.size());
	for (size_t i = 0; i < output_buffer_.size(); ++i) {
		if (output_buffer_[i] < 10 || static_cast<unsigned char>(output_buffer_[i]) > 127) {
			log_ += fztranslate("Received invalid character in version information") + L"\n";
			raw_version_information_.clear();
			return false;
		}
		raw_version_information_[i] = output_buffer_[i];
	}

	return true;
}

void CUpdater::ParseData()
{
	int64_t const ownVersionNumber = CBuildInfo::ConvertToVersionNumber(GetFileZillaVersion().c_str());
	version_information_ = version_information();

	std::wstring raw_version_information = raw_version_information_;

	log_ += fz::sprintf(_("Parsing %d bytes of version information.\n"), static_cast<int>(raw_version_information.size()));

	while (!raw_version_information.empty()) {
		std::wstring line;
		size_t pos = raw_version_information.find('\n');
		if (pos != std::wstring::npos) {
			line = raw_version_information.substr(0, pos);
			raw_version_information = raw_version_information.substr(pos + 1);
		}
		else {
			line = raw_version_information;
			raw_version_information.clear();
		}

		auto const tokens = fz::strtok(line, L" \t\r\n");
		if (tokens.empty()) {
			// After empty line, changelog follows
			version_information_.changelog_ = raw_version_information;
			fz::trim(version_information_.changelog_);

			if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
				log_ += fz::sprintf(L"Changelog: %s\n", version_information_.changelog_);
			}
			break;
		}

		std::wstring const& type = tokens[0];
		if (tokens.size() < 2) {
			if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
				log_ += fz::sprintf(L"Skipping line with one token of type %s\n", type);
			}
			continue;
		}

		if (type == L"resources") {
			if (UpdatableBuild()) {
				version_information_.resources_[resource_type::update_dialog] = tokens[1];
			}
			continue;
		}
		else if (type == "resource") {
			if (tokens.size() >= 3) {
				std::wstring resource;
				for (size_t i = 2; i < tokens.size(); ++i) {
					if (!resource.empty()) {
						resource += ' ';
					}
					resource += tokens[i];
				}
				version_information_.resources_[fz::to_integral<resource_type>(tokens[1])] = std::move(resource);
			}
			continue;
		}
		else if (type == L"eol") {
#if defined(__WXMSW__) || defined(__WXMAC__)
			std::string host = fz::to_utf8(CBuildInfo::GetHostname());
			if (host.empty()) {
				host = "unknown";
			}
			fz::to_utf8(GetFileZillaVersion());

			std::string data = host + '|' + fz::to_utf8(GetFileZillaVersion()) + '|' + fz::sprintf("%d.%d", wxPlatformInfo::Get().GetOSMajorVersion(), wxPlatformInfo::Get().GetOSMinorVersion());

			bool valid_signature{};
			for (size_t i = 1; i < tokens.size(); ++i) {
				auto const& token = tokens[i];
				if (token.substr(0, 4) == "sig:") {
					auto const& sig = token.substr(4);
					auto raw_sig = fz::base64_decode_s(fz::to_utf8(sig));

					if (!raw_sig.empty()) {
						auto const pub = fz::public_verification_key::from_base64("xrjuitldZT7pvIhK9q1GVNfptrepB/ctt5aK1QO5RaI");
						valid_signature = fz::verify(data, raw_sig, pub);
					}
				}
			}
			if (!valid_signature) {
				log_ += fz::sprintf(L"Ignoring eol statement not matching our version and platform.\n");
				continue;
			}
			version_information_.eol_ = true;
#endif
		}

		std::wstring const& versionOrDate = tokens[1];

		if (type == L"nightly") {
			fz::datetime nightlyDate(versionOrDate, fz::datetime::utc);
			if (nightlyDate.empty()) {
				if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
					log_ += L"Could not parse nightly date\n";
				}
				continue;
			}

			fz::datetime buildDate = CBuildInfo::GetBuildDate();
			if (buildDate.empty() || nightlyDate.empty() || nightlyDate <= buildDate) {
				if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
					log_ += L"Nightly isn't newer\n";
				}
				continue;
			}
		}
		else if (type == L"release" || type == L"beta") {
			int64_t v = CBuildInfo::ConvertToVersionNumber(versionOrDate.c_str());
			if (v <= ownVersionNumber) {
				continue;
			}
		}
		else {
			if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
				log_ += fz::sprintf(L"Skipping line with unknown type %s\n", type);
			}
			continue;
		}

		build b;
		b.version_ = versionOrDate;

		if (tokens.size() < 6) {
			if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
				log_ += fz::sprintf(L"Not parsing build line with only %d tokens", tokens.size());
			}
		}
		else if (UpdatableBuild()) {
			std::wstring const& url = tokens[2];
			std::wstring const& sizestr = tokens[3];
			std::wstring const& hash_algo = tokens[4];
			std::wstring const& hash = tokens[5];

			if (GetFilename(url).empty()) {
				if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
					log_ += fz::sprintf(L"Could not extract filename from URL: %s\n", url);
				}
				continue;
			}

			if (!fz::equal_insensitive_ascii(hash_algo, std::wstring(L"sha512"))) {
				continue;
			}

			auto const size = fz::to_integral<uint64_t>(sizestr);
			if (!size) {
				if (COptions::Get()->get_int(OPTION_LOGGING_DEBUGLEVEL) == 4) {
					log_ += fz::sprintf(L"Could not parse size: %s\n", sizestr);
				}
				continue;
			}

			bool valid_signature{};
			for (size_t i = 6; i < tokens.size(); ++i) {
				auto const& token = tokens[i];
				if (token.substr(0, 4) == "sig:") {
					auto const& sig = token.substr(4);
					auto raw_sig = fz::base64_decode(fz::to_utf8(sig));
					auto raw_hash = fz::hex_decode(hash);

					// Append the version to the file hash to protect against replays
					raw_hash.push_back(0);
					raw_hash.insert(raw_hash.cend(), versionOrDate.cbegin(), versionOrDate.cend());

					if (!raw_sig.empty() || !raw_hash.empty()) {
						auto const pub = fz::public_verification_key::from_base64("xrjuitldZT7pvIhK9q1GVNfptrepB/ctt5aK1QO5RaI");
						valid_signature = fz::verify(raw_hash, raw_sig, pub);
					}
				}
			}
			if (!valid_signature) {
				log_ += fz::sprintf(L"Ignoring line with inalid or missing signature for hash %s\n", hash);
				continue;
			}

			b.url_ = url;
			b.size_ = size;
			b.hash_ = fz::str_tolower_ascii(hash);
			bool valid_hash = true;
			for (auto const& c : b.hash_) {
				if ((c < 'a' || c > 'f') && (c < '0' || c > '9')) {
					valid_hash = false;
					break;
				}
			}
			if (!valid_hash) {
				log_ += fz::sprintf(_("Invalid hash: %s\n"), hash);
				continue;
			}

			// @translator: Two examples: Found new nightly 2014-04-03\n, Found new release 3.9.0.1\n
			log_ += fz::sprintf(_("Found new %s %s\n"), type, b.version_);
		}

		if (type == _T("nightly") && UpdatableBuild()) {
			version_information_.nightly_ = b;
		}
		else if (type == _T("release")) {
			version_information_.stable_ = b;
		}
		else if (type == _T("beta")) {
			version_information_.beta_ = b;
		}
	}

	version_information_.update_available();

	COptions::Get()->set(OPTION_UPDATECHECK_NEWVERSION, raw_version_information_);
}

void CUpdater::OnTimer(wxTimerEvent&)
{
	AutoRunIfNeeded();
}

bool CUpdater::VerifyChecksum(std::wstring const& file, int64_t size, std::wstring const& checksum)
{
	if (file.empty() || checksum.empty()) {
		return false;
	}

	auto filesize = fz::local_filesys::get_size(fz::to_native(file));
	if (filesize < 0) {
		log_ += fz::sprintf(_("Could not obtain size of '%s'"), file) + L"\n";
		return false;
	}
	else if (filesize != size) {
		log_ += fz::sprintf(_("Local size of '%s' does not match expected size: %d != %d"), file, filesize, size) + L"\n";
		return false;
	}

	fz::hash_accumulator acc(fz::hash_algorithm::sha512);

	{
		fz::file f(fz::to_native(file), fz::file::reading);
		if (!f.opened()) {
			log_ += fz::sprintf(_("Could not open '%s'"), file) + L"\n";
			return false;
		}
		unsigned char buffer[65536];
		int64_t read;
		while ((read = f.read(buffer, sizeof(buffer))) > 0) {
			acc.update(buffer, static_cast<size_t>(read));
		}
		if (read < 0) {
			log_ += fz::sprintf(_("Could not read from '%s'"), file) + L"\n";
			return false;
		}
	}

	auto const digest = fz::hex_encode<std::wstring>(acc.digest());

	if (digest != checksum) {
		log_ += fz::sprintf(_("Checksum mismatch on file %s\n"), file);
		return false;
	}

	log_ += fz::sprintf(_("Checksum match on file %s\n"), file);
	return true;
}

std::wstring CUpdater::GetTempFile() const
{
	wxASSERT(!version_information_.available_.hash_.empty());
	std::wstring ret = wxFileName::GetTempDir().ToStdWstring();
	if (!ret.empty()) {
		if (ret.back() != wxFileName::GetPathSeparator()) {
			ret += wxFileName::GetPathSeparator();
		}

		ret += L"fzupdate_" + version_information_.available_.hash_.substr(0, 16) + L".tmp";
	}

	return ret;
}

std::wstring CUpdater::GetFilename(std::wstring const& url) const
{
	std::wstring ret;
	size_t pos = url.rfind('/');
	if (pos != std::wstring::npos) {
		ret = url.substr(pos + 1);
	}
	size_t p = ret.find_first_of(_T("?#"));
	if (p != std::string::npos) {
		ret = ret.substr(0, p);
	}
#ifdef __WXMSW__
	fz::replace_substrings(ret, L":", L"_");
#endif

	return ret;
}

void CUpdater::SetState(UpdaterState s)
{
	if (s != state_) {
		state_ = s;

		if (s != UpdaterState::checking && s != UpdaterState::newversion_downloading) {
			pending_commands_.clear();
		}
		build b = version_information_.available_;
		for (auto const& handler : handlers_) {
			if (handler) {
				handler->UpdaterStateChanged(s, b);
			}
		}
	}
}

std::wstring CUpdater::DownloadedFile() const
{
	std::wstring ret;
	if (state_ == UpdaterState::newversion_ready) {
		ret = local_file_;
	}
	return ret;
}

void CUpdater::AddHandler(CUpdateHandler& handler)
{
	for (auto const& h : handlers_) {
		if (h == &handler) {
			return;
		}
	}
	for (auto& h : handlers_) {
		if (!h) {
			h = &handler;
			return;
		}
	}
	handlers_.push_back(&handler);
}

void CUpdater::RemoveHandler(CUpdateHandler& handler)
{
	for (auto& h : handlers_) {
		if (h == &handler) {
			// Set to 0 instead of removing from list to avoid issues with reentrancy.
			h = 0;
			return;
		}
	}
}

int64_t CUpdater::BytesDownloaded() const
{
	int64_t ret{-1};
	if (state_ == UpdaterState::newversion_ready) {
		if (!local_file_.empty()) {
			ret = fz::local_filesys::get_size(fz::to_native(local_file_));
		}
	}
	else if (state_ == UpdaterState::newversion_downloading) {
		std::wstring const temp = GetTempFile();
		if (!temp.empty()) {
			ret = fz::local_filesys::get_size(fz::to_native(temp));
		}
	}
	return ret;
}

bool CUpdater::UpdatableBuild() const
{
	return CBuildInfo::GetBuildType() == _T("nightly") || CBuildInfo::GetBuildType() == _T("official");
}

bool CUpdater::Busy() const
{
	return state_ == UpdaterState::checking || state_ == UpdaterState::newversion_downloading;
}

std::wstring CUpdater::GetResources(resource_type t) const
{
	std::wstring ret;
	auto const it = version_information_.resources_.find(t);
	if (it != version_information_.resources_.cend()) {
		ret = it->second;
	}
	return ret;
}

#endif
