/*
 * Project: curve
 * File Created: Monday, 17th September 2018 4:20:58 pm
 * Author: tongguangxun
 * Copyright (c) 2018 NetEase
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <algorithm>
#include <vector>
#include <string>
#include "src/client/splitor.h"
#include "src/client/mds_client.h"
#include "src/client/file_instance.h"
#include "src/client/request_closure.h"

namespace curve {
namespace client {
IOSplitOPtion_t Splitor::iosplitopt_;
void Splitor::Init(IOSplitOPtion_t ioSplitOpt) {
    iosplitopt_ = ioSplitOpt;
}
int Splitor::IO2ChunkRequests(IOTracker* iotracker,
                              MetaCache* mc,
                              std::list<RequestContext*>* targetlist,
                              const char* data,
                              off_t offset,
                              size_t length,
                              MDSClient* mdsclient,
                              const FInfo_t* fi) {
    if (targetlist == nullptr|| data == nullptr || mdsclient == nullptr ||
        mc == nullptr || iotracker == nullptr || fi == nullptr) {
        return -1;
    }

    uint64_t chunksize = fi->chunksize;

    uint64_t startchunkindex = offset / chunksize;
    uint64_t endchunkindex = (offset + length - 1) / chunksize;
    uint64_t startoffset = offset;
    uint64_t endoff = offset + length;
    uint64_t currentoff = offset % chunksize;
    uint64_t dataoff = 0;

    while (startchunkindex <= endchunkindex) {
        uint64_t off = currentoff;
        uint64_t chunkendpos = chunksize * (startchunkindex + 1);
        uint64_t pos = chunkendpos > endoff ? endoff : chunkendpos;
        uint64_t len = pos - startoffset;

        DVLOG(9) << "request split"
                 << ", off = " << off
                 << ", len = " << len
                 << ", seqnum = " << fi->seqnum
                 << ", endoff = " << endoff
                 << ", chunkendpos = " << chunkendpos
                 << ", chunksize = " << chunksize
                 << ", chunkindex = " << startchunkindex
                 << ", endchunkindex = " << endchunkindex;

        if (!AssignInternal(iotracker, mc, targetlist, data + dataoff,
                            off, len, mdsclient, fi, startchunkindex)) {
            LOG(ERROR) << "request split failed"
                       << ", off = " << off
                       << ", len = " << len
                       << ", seqnum = " << fi->seqnum
                       << ", chunksize = " << chunksize
                       << ", chunkindex = " << startchunkindex;
            return -1;
        }

        currentoff = 0;
        startchunkindex++;

        dataoff += len;
        startoffset += len;
    }
    return 0;
}

// this offset is begin by chunk
int Splitor::SingleChunkIO2ChunkRequests(IOTracker* iotracker,
                                        MetaCache* mc,
                                        std::list<RequestContext*>* targetlist,
                                        const ChunkIDInfo_t idinfo,
                                        const char* data,
                                        off_t offset,
                                        uint64_t length,
                                        uint64_t seq) {
    if (targetlist == nullptr || mc == nullptr ||
        iotracker == nullptr   || data == nullptr) {
            return -1;
    }

    auto max_split_size_bytes = 1024 * iosplitopt_.ioSplitMaxSizeKB;

    uint64_t len = 0;
    uint64_t off = 0;
    uint64_t tempoff = offset;
    uint64_t leftlength = length;
    while (leftlength > 0) {
        tempoff += len;
        len = leftlength > max_split_size_bytes ? max_split_size_bytes : leftlength;    // NOLINT

        RequestContext* newreqNode = new (std::nothrow) RequestContext();
        if (newreqNode == nullptr || !newreqNode->Init()) {
            return -1;
        }
        newreqNode->seq_         = seq;
        newreqNode->data_        = data + off;
        newreqNode->offset_      = tempoff;
        newreqNode->rawlength_   = len;
        newreqNode->optype_      = iotracker->Optype();
        newreqNode->idinfo_      = idinfo;
        newreqNode->done_->SetIOTracker(iotracker);
        targetlist->push_back(newreqNode);

        DVLOG(9) << "request split"
                 << ", off = " << tempoff
                 << ", len = " << len
                 << ", seqnum = " << seq
                 << ", chunkid = " << idinfo.cid_
                 << ", copysetid = " << idinfo.cpid_
                 << ", logicpoolid = " << idinfo.lpid_;

        leftlength -= len;
        off += len;
    }
    return 0;
}

bool Splitor::AssignInternal(IOTracker* iotracker,
                            MetaCache* mc,
                            std::list<RequestContext*>* targetlist,
                            const char* buf,
                            off_t off,
                            size_t len,
                            MDSClient* mdsclient,
                            const FInfo_t* fileinfo,
                            ChunkIndex chunkidx) {
    auto max_split_size_bytes = 1024 * iosplitopt_.ioSplitMaxSizeKB;

    ChunkIDInfo_t chinfo;
    SegmentInfo segInfo;
    LogicalPoolCopysetIDInfo_t lpcsIDInfo;
    MetaCacheErrorType chunkidxexist = mc->GetChunkInfoByIndex(chunkidx, &chinfo);          // NOLINT

    if (chunkidxexist == MetaCacheErrorType::CHUNKINFO_NOT_FOUND) {
        LIBCURVE_ERROR re = mdsclient->GetOrAllocateSegment(true,
                                        UserInfo(fileinfo->owner, ""),
                                        (off_t)chunkidx * fileinfo->chunksize,
                                        fileinfo,
                                        &segInfo);
        if (re == LIBCURVE_ERROR::FAILED || re == LIBCURVE_ERROR::AUTHFAIL) {
            LOG(ERROR) << "GetOrAllocateSegment failed!";
            return false;
        } else {
            int count = 0;
            for (auto chunkidinfo : segInfo.chunkvec) {
                uint64_t index = (segInfo.startoffset +
                         count * fileinfo->chunksize) / fileinfo->chunksize;
                mc->UpdateChunkInfoByIndex(index, chunkidinfo);
                ++count;
            }

            std::vector<CopysetInfo_t> cpinfoVec;
            re = mdsclient->GetServerList(segInfo.lpcpIDInfo.lpid,
                                         segInfo.lpcpIDInfo.cpidVec,
                                         &cpinfoVec);
            if (re == LIBCURVE_ERROR::FAILED) {
                LOG(ERROR) << "GetOrAllocateSegment failed!";
                return false;
            } else {
                for (auto cpinfo : cpinfoVec) {
                    mc->UpdateCopysetInfo(segInfo.lpcpIDInfo.lpid,
                    cpinfo.cpid_, cpinfo);
                }
            }
        }

        chunkidxexist = mc->GetChunkInfoByIndex(chunkidx, &chinfo);
    }
    if (chunkidxexist == MetaCacheErrorType::OK) {
        int ret = 0;
        auto appliedindex_ = mc->GetAppliedIndex(chinfo.lpid_, chinfo.cpid_);
        std::list<RequestContext*> templist;
        if (len > max_split_size_bytes) {
            ret = SingleChunkIO2ChunkRequests(iotracker, mc, &templist, chinfo,
                                              buf, off, len, fileinfo->seqnum);

            for_each(templist.begin(), templist.end(), [&](RequestContext* it) {
                it->appliedindex_ = appliedindex_;
            });

            targetlist->insert(targetlist->end(), templist.begin(), templist.end());    // NOLINT
        } else {
            RequestContext* newreqNode = new (std::nothrow) RequestContext();
            if (newreqNode == nullptr || !newreqNode->Init()) {
                return false;
            }
            newreqNode->seq_          = fileinfo->seqnum;
            newreqNode->data_         = buf;
            newreqNode->offset_       = off;
            newreqNode->rawlength_    = len;
            newreqNode->optype_       = iotracker->Optype();
            newreqNode->idinfo_       = chinfo;
            newreqNode->appliedindex_ = appliedindex_;
            newreqNode->done_->SetIOTracker(iotracker);

            targetlist->push_back(newreqNode);
        }
        return ret == 0;
    }
    LOG(ERROR) << "can not find the chunk index info!";
    return false;
}
}   // namespace client
}   // namespace curve
