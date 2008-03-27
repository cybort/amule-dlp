//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2008 aMule Team ( admin@amule.org / http://www.amule.org )
// Copyright (c) 2002 Merkur ( devs@emule-project.net / http://www.emule-project.net )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "updownclient.h"	// Interface

#include <protocol/Protocols.h>
#include <protocol/ed2k/Client2Client/TCP.h>

#include <zlib.h>

#include "ClientCredits.h"	// Needed for CClientCredits
#include "Packet.h"		// Needed for CPacket
#include "MemFile.h"		// Needed for CMemFile
#include "UploadQueue.h"	// Needed for CUploadQueue
#include "DownloadQueue.h"	// Needed for CDownloadQueue
#include "PartFile.h"		// Needed for PR_POWERSHARE
#include "ClientTCPSocket.h"	// Needed for CClientTCPSocket
#include "SharedFileList.h"	// Needed for CSharedFileList
#include "amule.h"		// Needed for theApp
#include "ClientList.h"
#include "Statistics.h"		// Needed for theStats
#include "Logger.h"
#include <common/Format.h>
#include "ScopedPtr.h"		// Needed for CScopedArray
#include "GuiEvents.h"		// Needed for Notify_*


//	members of CUpDownClient
//	which are mainly used for uploading functions 

void CUpDownClient::SetUploadState(uint8 eNewState)
{
	if (eNewState != m_nUploadState) {
		if (m_nUploadState == US_UPLOADING) {
			// Reset upload data rate computation
			m_nUpDatarate = 0;
			m_nSumForAvgUpDataRate = 0;
			m_AvarageUDR_list.clear();
		}
		if (eNewState == US_UPLOADING) {
			m_fSentOutOfPartReqs = 0;
		}

		// don't add any final cleanups for US_NONE here
		m_nUploadState = eNewState;
		UpdateDisplayedInfo(true);
	}
}

#ifndef CLIENT_GUI
uint32 CUpDownClient::GetScore(bool sysvalue, bool isdownloading, bool onlybasevalue) const
{
	//TODO: complete this (friends, uploadspeed, amuleuser etc etc)
	if (m_Username.IsEmpty()) {
		return 0;
	}

	if (credits == 0) {
		return 0;
	}

	const CKnownFile* pFile = GetUploadFile();
	if ( !pFile ) {
		return 0;
	}

	// bad clients (see note in function)
	if (IsBadGuy()) {
		return 0;
	}
	
	// friend slot
	if (IsFriend() && GetFriendSlot() && !HasLowID()) {
		return 0x0FFFFFFF;
	}

	if (IsBanned())
		return 0;

	if (sysvalue && HasLowID() && !IsConnected()){
		return 0;
	}	

	// TODO coded by tecxx & herbert, one yet unsolved problem here:
	// sometimes a client asks for 2 files and there is no way to decide, which file the 
	// client finally gets. so it could happen that he is queued first because of a 
	// high prio file, but then asks for something completely different.
	int filepriority = 10; // standard
	if(pFile != NULL){ 
		switch(pFile->GetUpPriority()) {
		        case PR_POWERSHARE: //added for powershare (deltaHF)
				filepriority = 2500; 
				break; //end
			case PR_VERYHIGH:
				filepriority = 18;
				break;
			case PR_HIGH:
				filepriority = 9;
				break;
			case PR_LOW:
				filepriority = 6;
				break;
			case PR_VERYLOW:
				filepriority = 2;
				break;
			case PR_NORMAL:
			default:
				filepriority = 7;
				break;
		}
	}
	// calculate score, based on waitingtime and other factors
	float fBaseValue;
	if (onlybasevalue) {
		fBaseValue = 100;
	} else if (!isdownloading) {
		fBaseValue = (float)(::GetTickCount()-GetWaitStartTime())/1000;
	} else {
		// we dont want one client to download forever
		// the first 15 min downloadtime counts as 15 min waitingtime and you get
		// a 15 min bonus while you are in the first 15 min :)
		// (to avoid 20 sec downloads) after this the score won't raise anymore 
		fBaseValue = (float)(m_dwUploadTime-GetWaitStartTime());
		wxASSERT( m_dwUploadTime > GetWaitStartTime()); // Obviously
		fBaseValue += (float)(::GetTickCount() - m_dwUploadTime > 900000)? 900000:1800000;
		fBaseValue /= 1000;
	}
	
	float modif = GetScoreRatio();
	fBaseValue *= modif;
	
	if (!onlybasevalue) {
		fBaseValue *= (float(filepriority)/10.0f);
	}
	if( (IsEmuleClient() || GetClientSoft() < 10) && m_byEmuleVersion <= 0x19) {
		fBaseValue *= 0.5f;
	}
	return (uint32)fBaseValue;
}
#endif

// Checks if it is next requested block from another chunk of the actual file or from another file 
// 
// [Returns] 
//   true : Next requested block is from another different chunk or file than last downloaded block 
//   false: Next requested block is from same chunk that last downloaded block 
bool CUpDownClient::IsDifferentPartBlock() const // [Tarod 12/22/2002] 
{ 
	bool different_part = false;
	
	// Check if we have good lists and proceed to check for different chunks
	if ((!m_BlockRequests_queue.empty()) && !m_DoneBlocks_list.empty())
	{
		Requested_Block_Struct* last_done_block = NULL;
		Requested_Block_Struct* next_requested_block = NULL;
		uint64 last_done_part = 0xffffffff;
		uint64 next_requested_part = 0xffffffff;
			
			
		// Get last block and next pending
		last_done_block = m_DoneBlocks_list.front();
		next_requested_block = m_BlockRequests_queue.front();
			
		// Calculate corresponding parts to blocks
		last_done_part = last_done_block->StartOffset / PARTSIZE;
		next_requested_part = next_requested_block->StartOffset / PARTSIZE; 
             
		// Test is we are asking same file and same part
		if ( last_done_part != next_requested_part) { 
			different_part = true;
			AddDebugLogLineM(false, logClient, wxT("Session ended due to new chunk."));
		}
	
		if (md4cmp(last_done_block->FileID, next_requested_block->FileID) != 0) { 
			different_part = true;
			AddDebugLogLineM(false, logClient, wxT("Session ended due to different file."));
		}
	} 

	return different_part; 
}


void CUpDownClient::CreateNextBlockPackage()
{
	// See if we can do an early return. There may be no new blocks to load from disk and add to buffer, or buffer may be large enough allready.
	if(m_BlockRequests_queue.empty() || // There are no new blocks requested
		m_addedPayloadQueueSession > GetQueueSessionPayloadUp() && m_addedPayloadQueueSession-GetQueueSessionPayloadUp() > 50*1024) { // the buffered data is large enough allready
		return;
	}

	CFile file;
	CPath fullname;
	try {
	// Buffer new data if current buffer is less than 100 KBytes
	while ((!m_BlockRequests_queue.empty()) &&
		(m_addedPayloadQueueSession <= GetQueueSessionPayloadUp() || m_addedPayloadQueueSession-GetQueueSessionPayloadUp() < 100*1024)) {

			Requested_Block_Struct* currentblock = m_BlockRequests_queue.front();
			CKnownFile* srcfile = theApp->sharedfiles->GetFileByID(CMD4Hash(currentblock->FileID));
			
			if (!srcfile) {
				throw wxString(wxT("requested file not found"));
			}

			if (srcfile->IsPartFile() && ((CPartFile*)srcfile)->GetStatus() != PS_COMPLETE) {
				//#warning This seems a good idea from eMule. We must import this.
				#if 0
				// Do not access a part file, if it is currently moved into the incoming directory.
				// Because the moving of part file into the incoming directory may take a noticable 
				// amount of time, we can not wait for 'm_FileCompleteMutex' and block the main thread.
				if (!((CPartFile*)srcfile)->m_FileCompleteMutex.Lock(0)){ // just do a quick test of the mutex's state and return if it's locked.
					return;
				}
				lockFile.m_pObject = &((CPartFile*)srcfile)->m_FileCompleteMutex;
				// If it's a part file which we are uploading the file remains locked until we've read the
				// current block. This way the file completion thread can not (try to) "move" the file into
				// the incoming directory.
				#endif

				// Get the full path to the '.part' file
				fullname = dynamic_cast<CPartFile*>(srcfile)->GetFullName().RemoveExt();
			} else {
				fullname = srcfile->GetFilePath().JoinPaths(srcfile->GetFileName());
			}
		
			uint64 togo;
			if (currentblock->StartOffset > currentblock->EndOffset){
				togo = currentblock->EndOffset + (srcfile->GetFileSize() - currentblock->StartOffset);
			} else {
				togo = currentblock->EndOffset - currentblock->StartOffset;
				
				if (srcfile->IsPartFile() && !((CPartFile*)srcfile)->IsComplete(currentblock->StartOffset,currentblock->EndOffset-1)) {
					throw wxString(wxT("Asked for incomplete block "));
				}
			}

			if (togo > EMBLOCKSIZE * 3) {
				throw wxString(wxT("Client requested too large of a block."));
			}
		
			CScopedArray<byte> filedata(NULL);	
			if (!srcfile->IsPartFile()){
				if ( !file.Open(fullname, CFile::read) ) {
					// The file was most likely moved/deleted. However it is likely that the
					// same is true for other files, so we recheck all shared files. 
					AddLogLineM( false, CFormat( _("Failed to open file (%s), removing from list of shared files.") ) % srcfile->GetFileName() );
					theApp->sharedfiles->RemoveFile(srcfile);
					
					throw wxString(wxT("Failed to open requested file: Removing from list of shared files!"));
				}			
			
				file.Seek(currentblock->StartOffset, wxFromStart);
				
				filedata.reset(new byte[togo + 500]);
				file.Read(filedata.get(), togo);
				file.Close();
			} else {
				CPartFile* partfile = (CPartFile*)srcfile;
				partfile->m_hpartfile.Seek(currentblock->StartOffset);
				
				filedata.reset(new byte[togo + 500]);
				partfile->m_hpartfile.Read(filedata.get(), togo); 
				// Partfile should NOT be closed!!!
			}

			//#warning Part of the above import.
			#if 0
			if (lockFile.m_pObject){
				lockFile.m_pObject->Unlock(); // Unlock the (part) file as soon as we are done with accessing it.
				lockFile.m_pObject = NULL;
			}
			#endif
	
			SetUploadFileID(srcfile);

			// check extention to decide whether to compress or not
			if (m_byDataCompVer == 1 && GetFiletype(srcfile->GetFileName()) != ftArchive) {
				CreatePackedPackets(filedata.get(), togo,currentblock);
			} else {
				CreateStandartPackets(filedata.get(), togo,currentblock);
			}
			
			// file statistic
			srcfile->statistic.AddTransferred(togo);

			m_addedPayloadQueueSession += togo;

			Requested_Block_Struct* block = m_BlockRequests_queue.front();
			
			m_BlockRequests_queue.pop_front();			
			m_DoneBlocks_list.push_front(block);
		}

		return;
	} catch (const wxString& error) {
		AddDebugLogLineM(false, logClient, wxT("Client '") + GetUserName() + wxT("' caused error while creating packet (") + error + wxT(") - disconnecting client"));
	} catch (const CIOFailureException& error) {
		AddDebugLogLineM(true, logClient, wxT("IO failure while reading requested file: ") + error.what());
	} catch (const CEOFException& error) {
		AddDebugLogLineM(true, logClient, GetClientFullInfo() + wxT(" requested file-data at an invalid position - disconnecting"));
	}
	
	// Error occured.	
	theApp->uploadqueue->RemoveFromUploadQueue(this);
}


void CUpDownClient::CreateStandartPackets(const byte* buffer, uint32 togo, Requested_Block_Struct* currentblock)
{
	uint32 nPacketSize;

	CMemFile memfile(buffer, togo);
	if (togo > 10240) {
		nPacketSize = togo/(uint32)(togo/10240);
	} else {
		nPacketSize = togo;
	}
	
	while (togo){
		if (togo < nPacketSize*2) {
			nPacketSize = togo;
		}
		
		wxASSERT(nPacketSize);
		togo -= nPacketSize;
		
		uint64 endpos = (currentblock->EndOffset - togo);
		uint64 startpos = endpos - nPacketSize;
		
		bool bLargeBlocks = (startpos > 0xFFFFFFFF) || (endpos > 0xFFFFFFFF);
		
		CMemFile data(nPacketSize + 16 + 2 * (bLargeBlocks ? 8 :4));
		data.WriteHash(GetUploadFileID());
		if (bLargeBlocks) {
			data.WriteUInt64(startpos);
			data.WriteUInt64(endpos);
		} else {
			data.WriteUInt32(startpos);
			data.WriteUInt32(endpos);
		}
		char *tempbuf = new char[nPacketSize];
		memfile.Read(tempbuf, nPacketSize);
		data.Write(tempbuf, nPacketSize);
		delete [] tempbuf;
		CPacket* packet = new CPacket(data, (bLargeBlocks ? OP_EMULEPROT : OP_EDONKEYPROT), (bLargeBlocks ? (uint8)OP_SENDINGPART_I64 : (uint8)OP_SENDINGPART));	
		theStats::AddUpOverheadFileRequest(16 + 2 * (bLargeBlocks ? 8 :4));
		theStats::AddUploadToSoft(GetClientSoft(), nPacketSize);
		AddDebugLogLineM( false, logLocalClient, wxString::Format(wxT("Local Client: %s to "),(bLargeBlocks ? wxT("OP_SENDINGPART_I64") : wxT("OP_SENDINGPART"))) + GetFullIP() );
		m_socket->SendPacket(packet,true,false, nPacketSize);
	}
}


void CUpDownClient::CreatePackedPackets(const byte* buffer, uint32 togo, Requested_Block_Struct* currentblock)
{
	byte* output = new byte[togo+300];
	uLongf newsize = togo+300;
	uint16 result = compress2(output, &newsize, buffer, togo,9);
	if (result != Z_OK || togo <= newsize){
		delete[] output;
		CreateStandartPackets(buffer, togo, currentblock);
		return;
	}
	
	CMemFile memfile(output,newsize);
	
	uint32 totalPayloadSize = 0;
	uint32 oldSize = togo;
	togo = newsize;
	uint32 nPacketSize;
	if (togo > 10240) {
		nPacketSize = togo/(uint32)(togo/10240);
	} else {
		nPacketSize = togo;
	}
		
	while (togo) {
		if (togo < nPacketSize*2) {
			nPacketSize = togo;
		}
		togo -= nPacketSize;

		bool isLargeBlock = (currentblock->StartOffset > 0xFFFFFFFF) || (currentblock->EndOffset > 0xFFFFFFFF);
		
		CMemFile data(nPacketSize + 16 + (isLargeBlock ? 12 : 8));
		data.WriteHash(GetUploadFileID());
		if (isLargeBlock) {
			data.WriteUInt64(currentblock->StartOffset);
		} else {
			data.WriteUInt32(currentblock->StartOffset);
		}
		data.WriteUInt32(newsize);			
		char *tempbuf = new char[nPacketSize];
		memfile.Read(tempbuf, nPacketSize);
		data.Write(tempbuf,nPacketSize);
		delete [] tempbuf;
		CPacket* packet = new CPacket(data, OP_EMULEPROT, (isLargeBlock ? OP_COMPRESSEDPART_I64 : OP_COMPRESSEDPART));
	
		// approximate payload size
		uint32 payloadSize = nPacketSize*oldSize/newsize;

		if (togo == 0 && totalPayloadSize+payloadSize < oldSize) {
			payloadSize = oldSize-totalPayloadSize;
		}
		
		totalPayloadSize += payloadSize;

		// put packet directly on socket
		theStats::AddUpOverheadFileRequest(24);
		theStats::AddUploadToSoft(GetClientSoft(), nPacketSize);
		AddDebugLogLineM( false, logLocalClient, wxString::Format(wxT("Local Client: %s to "), (isLargeBlock ? wxT("OP_COMPRESSEDPART_I64") : wxT("OP_COMPRESSEDPART"))) + GetFullIP() );
		m_socket->SendPacket(packet,true,false, payloadSize);			
	}
	delete[] output;
}


void CUpDownClient::ProcessExtendedInfo(const CMemFile *data, CKnownFile *tempreqfile)
{
	m_uploadingfile->UpdateUpPartsFrequency( this, false ); // Decrement
	m_upPartStatus.clear();		
	m_nUpCompleteSourcesCount= 0;
	
	if( GetExtendedRequestsVersion() == 0 ) {
		// Something is coded wrong on this client if he's sending something it doesn't advertise.
		return;
	}
	
	if (data->GetLength() == 16) {
		// Wrong again. Advertised >0 but send a 0-type packet.
		// But this time we'll disconnect it.
		throw CInvalidPacket(wxT("Wrong size on extended info packet"));
	}
	
	uint16 nED2KUpPartCount = data->ReadUInt16();
	if (!nED2KUpPartCount) {
		m_upPartStatus.resize( tempreqfile->GetPartCount(), 0 );
	} else {
		if (tempreqfile->GetED2KPartCount() != nED2KUpPartCount) {
			// We already checked if we are talking about the same file.. So if we get here, something really strange happened!
			m_upPartStatus.clear();
			return;
		}
	
		m_upPartStatus.resize( tempreqfile->GetPartCount(), 0 );
	
		try {
			uint16 done = 0;
			while (done != m_upPartStatus.size()) {
				uint8 toread = data->ReadUInt8();
				for (sint32 i = 0;i != 8;i++){
					m_upPartStatus[done] = (toread>>i)&1;
					//	We may want to use this for another feature..
					//	if (m_upPartStatus[done] && !tempreqfile->IsComplete(done*PARTSIZE,((done+1)*PARTSIZE)-1))
					// bPartsNeeded = true;
					done++;
					if (done == m_upPartStatus.size()) {
						break;
					}
				}
			}
		} catch (...) {
			// We want the increment the frequency even if we didn't read everything
			m_uploadingfile->UpdateUpPartsFrequency( this, true ); // Increment
			
			throw;
		}

		if (GetExtendedRequestsVersion() > 1) {
			uint16 nCompleteCountLast = GetUpCompleteSourcesCount();
			uint16 nCompleteCountNew = data->ReadUInt16();
			SetUpCompleteSourcesCount(nCompleteCountNew);
			if (nCompleteCountLast != nCompleteCountNew) {
				tempreqfile->UpdatePartsInfo();
			}
		}
	}
	
	m_uploadingfile->UpdateUpPartsFrequency( this, true ); // Increment
	
	Notify_QlistRefreshClient(this);
}


void CUpDownClient::SetUploadFileID(CKnownFile* newreqfile)
{
	if (m_uploadingfile == newreqfile) {
		return;
	} else if (m_uploadingfile) {
		m_uploadingfile->RemoveUploadingClient(this);
		m_uploadingfile->UpdateUpPartsFrequency(this, false); // Decrement
	}
	
	if (newreqfile) {
		// This is a new file! update info
		newreqfile->AddUploadingClient(this);
		
		if (m_requpfileid != newreqfile->GetFileHash()) {
			m_requpfileid = newreqfile->GetFileHash();
			m_upPartStatus.clear();
			m_upPartStatus.resize( newreqfile->GetPartCount(), 0 );
		} else {
			// this is the same file we already had assigned. Only update data.
			newreqfile->UpdateUpPartsFrequency(this, true); // Increment
 		}
		
		m_uploadingfile = newreqfile;
	} else {
		m_upPartStatus.clear();
		m_nUpCompleteSourcesCount = 0;
		// This clears m_uploadingfile and m_requpfileid
		ClearUploadFileID();
	}
}


void CUpDownClient::AddReqBlock(Requested_Block_Struct* reqblock)
{
	if (GetUploadState() != US_UPLOADING) {
		AddDebugLogLineM(false, logRemoteClient, wxT("UploadClient: Client tried to add requested block when not in upload slot! Prevented requested blocks from being added."));
		delete reqblock;
		return;
	}
	
	{
		std::list<Requested_Block_Struct*>::iterator it = m_DoneBlocks_list.begin();
		for (; it != m_DoneBlocks_list.end(); ++it) {
			if (reqblock->StartOffset == (*it)->StartOffset && reqblock->EndOffset == (*it)->EndOffset) {
				delete reqblock;
				return;
			}
		}
	}

	{
		std::list<Requested_Block_Struct*>::iterator it = m_BlockRequests_queue.begin();
		for (; it != m_BlockRequests_queue.end(); ++it) {
			if (reqblock->StartOffset == (*it)->StartOffset && reqblock->EndOffset == (*it)->EndOffset) {
				delete reqblock;
				return;
			}
		}
	}
	
	m_BlockRequests_queue.push_back(reqblock);
}


uint32 CUpDownClient::GetWaitStartTime() const
{
	uint32 dwResult = 0;
	
	if ( credits ) {
		dwResult = credits->GetSecureWaitStartTime(GetIP());
		
		if (dwResult > m_dwUploadTime && IsDownloading()) {
			// This happens only if two clients with invalid securehash are in the queue - if at all
			dwResult = m_dwUploadTime - 1;
		}
	}
		
	return dwResult;
}


void CUpDownClient::SetWaitStartTime()
{
	if ( credits ) {
		credits->SetSecWaitStartTime(GetIP());
	}
}


void CUpDownClient::ClearWaitStartTime()
{
	if ( credits ) {
		credits->ClearWaitStartTime();
	}
}


uint32 CUpDownClient::SendBlockData()
{
    uint32 curTick = ::GetTickCount();
    uint64 sentBytesCompleteFile = 0;
    uint64 sentBytesPartFile = 0;
    uint64 sentBytesPayload = 0;

    if (m_socket) {
		CEMSocket* s = m_socket;
//		uint32 uUpStatsPort = GetUserPort();

	    // Extended statistics information based on which client software and which port we sent this data to...
	    // This also updates the grand total for sent bytes, etc.  And where this data came from.
        sentBytesCompleteFile = s->GetSentBytesCompleteFileSinceLastCallAndReset();
        sentBytesPartFile = s->GetSentBytesPartFileSinceLastCallAndReset();
//		thePrefs.Add2SessionTransferData(GetClientSoft(), uUpStatsPort, false, true, sentBytesCompleteFile, (IsFriend() && GetFriendSlot()));
//		thePrefs.Add2SessionTransferData(GetClientSoft(), uUpStatsPort, true, true, sentBytesPartFile, (IsFriend() && GetFriendSlot()));

		m_nTransferredUp += sentBytesCompleteFile + sentBytesPartFile;
        credits->AddUploaded(sentBytesCompleteFile + sentBytesPartFile, GetIP(), theApp->CryptoAvailable());

        sentBytesPayload = s->GetSentPayloadSinceLastCallAndReset();
        m_nCurQueueSessionPayloadUp += sentBytesPayload;

        if (theApp->uploadqueue->CheckForTimeOver(this)) {
            theApp->uploadqueue->RemoveFromUploadQueue(this, true);
			SendOutOfPartReqsAndAddToWaitingQueue();
        } else {
            // read blocks from file and put on socket
            CreateNextBlockPackage();
        }
    }

    if(sentBytesCompleteFile + sentBytesPartFile > 0 ||
        m_AvarageUDR_list.empty() || (curTick - m_AvarageUDR_list.back().timestamp) > 1*1000) {
        // Store how much data we've transferred this round,
        // to be able to calculate average speed later
        // keep sum of all values in list up to date
        TransferredData newitem = {sentBytesCompleteFile + sentBytesPartFile, curTick};
        m_AvarageUDR_list.push_back(newitem);
        m_nSumForAvgUpDataRate += sentBytesCompleteFile + sentBytesPartFile;
    }

    // remove to old values in list
    while ((!m_AvarageUDR_list.empty()) && (curTick - m_AvarageUDR_list.front().timestamp) > 10*1000) {
        // keep sum of all values in list up to date
        m_nSumForAvgUpDataRate -= m_AvarageUDR_list.front().datalen;
		m_AvarageUDR_list.pop_front();
    }

    // Calculate average speed for this slot
    if ((!m_AvarageUDR_list.empty()) && (curTick - m_AvarageUDR_list.front().timestamp) > 0 && GetUpStartTimeDelay() > 2*1000) {
        m_nUpDatarate = ((uint64)m_nSumForAvgUpDataRate*1000) / (curTick-m_AvarageUDR_list.front().timestamp);
    } else {
        // not enough values to calculate trustworthy speed. Use -1 to tell this
        m_nUpDatarate = 0; //-1;
    }

    // Check if it's time to update the display.
	m_cSendblock++;
	if (m_cSendblock == 30){
		m_cSendblock = 0;
		Notify_UploadCtrlRefreshClient(this);
	}

    return sentBytesCompleteFile + sentBytesPartFile;
}


void CUpDownClient::SendOutOfPartReqsAndAddToWaitingQueue()
{
	// Kry - this is actually taken from eMule, but makes a lot of sense ;)
	
	//OP_OUTOFPARTREQS will tell the downloading client to go back to OnQueue..
	//The main reason for this is that if we put the client back on queue and it goes
	//back to the upload before the socket times out... We get a situation where the
	//downloader thinks it already sent the requested blocks and the uploader thinks
	//the downloader didn't send any request blocks. Then the connection times out..
	//I did some tests with eDonkey also and it seems to work well with them also..
	
	// Send this inmediately, don't queue.
	CPacket* pPacket = new CPacket(OP_OUTOFPARTREQS, 0, OP_EDONKEYPROT);
	theStats::AddUpOverheadFileRequest(pPacket->GetPacketSize());
	AddDebugLogLineM( false, logLocalClient, wxT("Local Client: OP_OUTOFPARTREQS to ") + GetFullIP() );
	SendPacket(pPacket, true, true);
	
	theApp->uploadqueue->AddClientToQueue(this);
}


/**
 * See description for CEMSocket::TruncateQueues().
 */
void CUpDownClient::FlushSendBlocks()
{
	// Call this when you stop upload, or the socket might be not able to send
    if (m_socket) {    //socket may be NULL...
        m_socket->TruncateQueues();
	}
}


void CUpDownClient::SendHashsetPacket(const CMD4Hash& forfileid)
{
	CKnownFile* file = theApp->sharedfiles->GetFileByID( forfileid );
	bool from_dq = false;
	if ( !file ) {
		from_dq = true;
		if ((file = theApp->downloadqueue->GetFileByID(forfileid)) == NULL) {
			AddLogLineM(false, CFormat( _("Hashset requested for unknown file: %s") ) % forfileid.Encode() );
		
			return;
		}
	}
	
	if ( !file->GetHashCount() ) {
		if (from_dq) {
			AddDebugLogLineM(false, logRemoteClient, wxT("Requested hashset could not be found"));	
			return;
		} else {
			file = theApp->downloadqueue->GetFileByID(forfileid);
			if (!(file && file->GetHashCount())) {
				AddDebugLogLineM(false, logRemoteClient, wxT("Requested hashset could not be found"));	
				return;				
			}
		}
	}	

	CMemFile data(1024);
	data.WriteHash(file->GetFileHash());
	uint16 parts = file->GetHashCount();
	data.WriteUInt16(parts);
	for (int i = 0; i != parts; i++) {
		data.WriteHash(file->GetPartHash(i));
	}
	CPacket* packet = new CPacket(data, OP_EDONKEYPROT, OP_HASHSETANSWER);	
	theStats::AddUpOverheadFileRequest(packet->GetPacketSize());
	AddDebugLogLineM( false, logLocalClient, wxT("Local Client: OP_HASHSETANSWER to ") + GetFullIP());
	SendPacket(packet,true,true);
}


void CUpDownClient::ClearUploadBlockRequests()
{
	FlushSendBlocks();
	DeleteContents(m_BlockRequests_queue);
	DeleteContents(m_DoneBlocks_list);
}


void CUpDownClient::SendRankingInfo(){
	if (!ExtProtocolAvailable()) {
		return;
	}

	uint16 nRank = theApp->uploadqueue->GetWaitingPosition(this);
	if (!nRank) {
		return;
	}
		
	CMemFile data;
	data.WriteUInt16(nRank);
	// Kry: what are these zero bytes for. are they really correct?
	// Kry - Well, eMule does like that. I guess they're ok.
	data.WriteUInt32(0); data.WriteUInt32(0); data.WriteUInt16(0);
	CPacket* packet = new CPacket(data, OP_EMULEPROT, OP_QUEUERANKING);
	theStats::AddUpOverheadOther(packet->GetPacketSize());
	AddDebugLogLineM(false, logLocalClient, wxT("Local Client: OP_QUEUERANKING to ") + GetFullIP());
	SendPacket(packet,true,true);
}


void CUpDownClient::SendCommentInfo(CKnownFile* file)
{
	if (!m_bCommentDirty || file == NULL || !ExtProtocolAvailable() || m_byAcceptCommentVer < 1) {
		return;
	}
	m_bCommentDirty = false;

	// Truncate to max len.
	wxString desc = file->GetFileComment().Left(MAXFILECOMMENTLEN);
	uint8 rating = file->GetFileRating();
	
	if ( file->GetFileRating() == 0 && desc.IsEmpty() ) {
		return;
	}
	
	CMemFile data(256);
	data.WriteUInt8(rating);
	data.WriteString(desc, GetUnicodeSupport(), 4 /* size it's uint32 */);
	
	CPacket* packet = new CPacket(data, OP_EMULEPROT, OP_FILEDESC);
	theStats::AddUpOverheadOther(packet->GetPacketSize());
	AddDebugLogLineM(false, logLocalClient, wxT("Local Client: OP_FILEDESC to ") + GetFullIP());	
	SendPacket(packet,true);
}

void  CUpDownClient::UnBan(){
	m_Aggressiveness = 0;
	
	theApp->clientlist->AddTrackClient(this);
	theApp->clientlist->RemoveBannedClient( GetIP() );
	SetUploadState(US_NONE);
	ClearWaitStartTime();
	
	Notify_ShowQueueCount(theStats::GetWaitingUserCount());
}

void CUpDownClient::Ban(){
	theApp->clientlist->AddTrackClient(this);
	theApp->clientlist->AddBannedClient( GetIP() );
	
	AddDebugLogLineM( false, logClient, wxT("Client '") + GetUserName() + wxT("' seems to be an aggressive client and is banned from the uploadqueue"));
	
	SetUploadState(US_BANNED);
	
	Notify_ShowQueueCount(theStats::GetWaitingUserCount());
	Notify_QlistRefreshClient(this);
}

bool CUpDownClient::IsBanned() const
{
	return ( (theApp->clientlist->IsBannedClient(GetIP()) ) && m_nDownloadState != DS_DOWNLOADING);
}

void CUpDownClient::CheckForAggressive()
{
	uint32 cur_time = ::GetTickCount();
	
	// First call, initalize
	if ( !m_LastFileRequest ) {
		m_LastFileRequest = cur_time;
		return;
	}
	
	// Is this an aggressive request?
	if ( ( cur_time - m_LastFileRequest ) < MIN_REQUESTTIME ) {
		m_Aggressiveness += 3;
		
		// Is the client EVIL?
		if ( m_Aggressiveness >= 10 && (!IsBanned() && m_nDownloadState != DS_DOWNLOADING )) {
			AddDebugLogLineM( false, logClient, CFormat( wxT("Aggressive client banned (score: %d): %s -- %s -- %s") ) 
				% m_Aggressiveness
				% m_Username
				% m_strModVersion
				% m_fullClientVerString );
			Ban();
		}
	} else {
		// Polite request, reward client
		if ( m_Aggressiveness )
			m_Aggressiveness--;
	}

	m_LastFileRequest = cur_time;
}


void CUpDownClient::SetUploadFileID(const CMD4Hash& new_id)
{
	// Update the uploading file found 
	CKnownFile* uploadingfile = theApp->sharedfiles->GetFileByID(new_id);
	if ( !uploadingfile ) {
		// Can this really happen?
		uploadingfile = theApp->downloadqueue->GetFileByID(new_id);
	}
	SetUploadFileID(uploadingfile); // This will update queue count on old and new file.
}

void CUpDownClient::ProcessRequestPartsPacket(const byte* pachPacket, uint32 nSize, bool largeblocks) {
	
	CMemFile data(pachPacket, nSize);
	
	CMD4Hash reqfilehash = data.ReadHash();
	
	uint64 auStartOffsets[3];
	uint64 auEndOffsets[3];
	
	if (largeblocks) {
		auStartOffsets[0] = data.ReadUInt64();
		auStartOffsets[1] = data.ReadUInt64();
		auStartOffsets[2] = data.ReadUInt64();
		
		auEndOffsets[0] = data.ReadUInt64();
		auEndOffsets[1] = data.ReadUInt64();
		auEndOffsets[2] = data.ReadUInt64();
	} else {
		auStartOffsets[0] = data.ReadUInt32();
		auStartOffsets[1] = data.ReadUInt32();
		auStartOffsets[2] = data.ReadUInt32();
		
		auEndOffsets[0] = data.ReadUInt32();
		auEndOffsets[1] = data.ReadUInt32();
		auEndOffsets[2] = data.ReadUInt32();		
	}
	
	for (unsigned int i = 0; i < itemsof(auStartOffsets); i++) {
		AddDebugLogLineM(false, logClient,
			wxString::Format(wxT("Client requests %u"), i)
			+ wxT(" ") + wxString::Format(wxT("File block %u-%u (%d bytes):"), auStartOffsets[i], auEndOffsets[i], auEndOffsets[i] - auStartOffsets[i])
			+ wxT(" ") + GetFullIP());
		if (auEndOffsets[i] > auStartOffsets[i]) {
			Requested_Block_Struct* reqblock = new Requested_Block_Struct;
			reqblock->StartOffset = auStartOffsets[i];
			reqblock->EndOffset = auEndOffsets[i];
			md4cpy(reqblock->FileID, reqfilehash.GetHash());
			reqblock->transferred = 0;
			AddReqBlock(reqblock);
		} else {
			if (auEndOffsets[i] != 0 || auStartOffsets[i] != 0) {
				AddDebugLogLineM(false, logClient, wxT("Client request is invalid!"));
			}
		}
	}	
}

void CUpDownClient::ProcessRequestPartsPacketv2(const CMemFile& data) {
	
	CMD4Hash reqfilehash = data.ReadHash();
	
	uint8 numblocks = data.ReadUInt8();
	
	for (int i = 0; i < numblocks; i++) {		
		Requested_Block_Struct* reqblock = new Requested_Block_Struct;
		try {
			reqblock->StartOffset = data.GetIntTagValue();
			// We have to do +1, because the block matching uses that.
			reqblock->EndOffset = data.GetIntTagValue() + 1;
			if ((reqblock->StartOffset || reqblock->EndOffset) && (reqblock->StartOffset > reqblock->EndOffset)) {
				AddDebugLogLineM(false, logClient, wxString::Format(wxT("Client request is invalid! %i / %i"),reqblock->StartOffset, reqblock->EndOffset));
				throw wxString(wxT("Client request is invalid!"));
			}
			
			AddDebugLogLineM(false, logClient,
				wxString::Format(wxT("Client requests %u"), i)
				+ wxT(" ") + wxString::Format(wxT("File block %u-%u (%d bytes):"),reqblock->StartOffset, reqblock->EndOffset, reqblock->EndOffset - reqblock->StartOffset)
				+= wxT(" ") + GetFullIP());
			
			md4cpy(reqblock->FileID, reqfilehash.GetHash());
			reqblock->transferred = 0;
			AddReqBlock(reqblock);
		} catch (...) {
			delete reqblock;
			throw;
		}
	}
}
// File_checked_for_headers