/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "peerconnection.hpp"
#include "dtlstransport.hpp"
#include "icetransport.hpp"
#include "sctptransport.hpp"

namespace rtc {

using namespace std::placeholders;

using std::function;
using std::shared_ptr;

PeerConnection::PeerConnection(const IceConfiguration &config)
    : mConfig(config), mCertificate(make_certificate("libdatachannel")), mMid("0"),
      mSctpPort(5000) {}

PeerConnection::~PeerConnection() {}

const IceConfiguration *PeerConnection::config() const { return &mConfig; }

const Certificate *PeerConnection::certificate() const { return &mCertificate; }

void PeerConnection::setRemoteDescription(const string &description) {
	Description desc(Description::Role::ActPass, description);

	if (auto fingerprint = desc.fingerprint())
		mRemoteFingerprint.emplace(*fingerprint);

	if (auto sctpPort = desc.sctpPort()) {
		mSctpPort = *sctpPort;
	}

	if (!mIceTransport) {
		initIceTransport(Description::Role::ActPass);
		mIceTransport->setRemoteDescription(desc);
		triggerLocalDescription();
		mIceTransport->gatherLocalCandidates();
	} else {
		mIceTransport->setRemoteDescription(desc);
	}
}

void PeerConnection::setRemoteCandidate(const string &candidate) {
	Candidate cand(candidate, mMid);
	if (mIceTransport) {
		mIceTransport->addRemoteCandidate(cand);
	}
}

shared_ptr<DataChannel> PeerConnection::createDataChannel(const string &label,
                                                          const string &protocol,
                                                          const Reliability &reliability) {
	// The active side must use streams with even identifiers, whereas the passive side must use
	// streams with odd identifiers.
	// See https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09#section-6
	auto role = mIceTransport ? mIceTransport->role() : Description::Role::Active;
	unsigned int stream = (role == Description::Role::Active) ? 0 : 1;
	while (mDataChannels.find(stream) != mDataChannels.end()) {
		stream += 2;
		if (stream >= 65535)
			throw std::runtime_error("Too many DataChannels");
	}

	auto channel = std::make_shared<DataChannel>(stream, label, protocol, reliability);
	mDataChannels.insert(std::make_pair(stream, channel));

	if (!mIceTransport) {
		initIceTransport(Description::Role::Active);
		triggerLocalDescription();
		mIceTransport->gatherLocalCandidates();
	} else if (mSctpTransport && mSctpTransport->isReady()) {
		channel->open(mSctpTransport);
	}
	return channel;
}

void PeerConnection::onDataChannel(
    std::function<void(std::shared_ptr<DataChannel> dataChannel)> callback) {
	mDataChannelCallback = callback;
}

void PeerConnection::onLocalDescription(std::function<void(const string &description)> callback) {
	mLocalDescriptionCallback = callback;
}

void PeerConnection::onLocalCandidate(
    std::function<void(const std::optional<string> &candidate)> callback) {
	mLocalCandidateCallback = callback;
}

void PeerConnection::initIceTransport(Description::Role role) {
	mIceTransport = std::make_shared<IceTransport>(
	    mConfig, role, std::bind(&PeerConnection::triggerLocalCandidate, this, _1),
	    std::bind(&PeerConnection::initDtlsTransport, this));
}

void PeerConnection::initDtlsTransport() {
	mDtlsTransport = std::make_shared<DtlsTransport>(
	    mIceTransport, mCertificate, std::bind(&PeerConnection::checkFingerprint, this, _1),
	    std::bind(&PeerConnection::initSctpTransport, this));
}

void PeerConnection::initSctpTransport() {
	mSctpTransport = std::make_shared<SctpTransport>(
	    mDtlsTransport, mSctpPort, std::bind(&PeerConnection::openDataChannels, this),
	    std::bind(&PeerConnection::forwardMessage, this, _1));
}

bool PeerConnection::checkFingerprint(const std::string &fingerprint) const {
	return mRemoteFingerprint && *mRemoteFingerprint == fingerprint;
}

void PeerConnection::forwardMessage(message_ptr message) {
	if (!mIceTransport || !mSctpTransport)
		throw std::logic_error("Got a DataChannel message without transport");

	shared_ptr<DataChannel> channel;
	if (auto it = mDataChannels.find(message->stream); it != mDataChannels.end()) {
		channel = it->second;
	} else {
		const byte dataChannelOpenMessage{0x03};
		unsigned int remoteParity = (mIceTransport->role() == Description::Role::Active) ? 1 : 0;
		if (message->type == Message::Control && *message->data() == dataChannelOpenMessage &&
		    message->stream % 2 == remoteParity) {
			channel = std::make_shared<DataChannel>(message->stream, mSctpTransport);
			channel->onOpen(std::bind(&PeerConnection::triggerDataChannel, this, channel));
			mDataChannels.insert(std::make_pair(message->stream, channel));
		} else {
			// Invalid, close the DataChannel by resetting the stream
			mSctpTransport->reset(message->stream);
			return;
		}
	}

	channel->incoming(message);
}

void PeerConnection::openDataChannels(void) {
	for (const auto &[stream, dataChannel] : mDataChannels)
		dataChannel->open(mSctpTransport);
}

void PeerConnection::triggerLocalDescription() {
	if (mLocalDescriptionCallback && mIceTransport) {
		Description desc{mIceTransport->getLocalDescription()};
		desc.setFingerprint(mCertificate.fingerprint());
		desc.setSctpPort(mSctpPort);
		mLocalDescriptionCallback(string(desc));
	}
}

void PeerConnection::triggerLocalCandidate(const std::optional<Candidate> &candidate) {
	if (mLocalCandidateCallback) {
		mLocalCandidateCallback(candidate ? std::make_optional(string(*candidate)) : nullopt);
	}
}

void PeerConnection::triggerDataChannel(std::shared_ptr<DataChannel> dataChannel) {
	if (mDataChannelCallback)
		mDataChannelCallback(dataChannel);
}

} // namespace rtc