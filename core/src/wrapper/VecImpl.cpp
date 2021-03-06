// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "wrapper/VecImpl.h"

#include "DataTransfer.h"
#include "knowhere/adapter/VectorAdapter.h"
#include "knowhere/common/Exception.h"
#include "knowhere/index/vector_index/IndexIDMAP.h"
#include "utils/Log.h"
#include "wrapper/WrapperException.h"
#include "wrapper/gpu/GPUVecImpl.h"

#ifdef MILVUS_GPU_VERSION

#include "knowhere/index/vector_index/IndexGPUIVF.h"
#include "knowhere/index/vector_index/IndexIVFSQHybrid.h"
#include "knowhere/index/vector_index/helpers/Cloner.h"

#endif

#include <fiu-local.h>
/*
 * no parameter check in this layer.
 * only responsible for index combination
 */

namespace milvus {
namespace engine {

Status
VecIndexImpl::BuildAll(const int64_t& nb, const float* xb, const int64_t* ids, const Config& cfg, const int64_t& nt,
                       const float* xt) {
    try {
        dim = cfg[knowhere::meta::DIM];
        auto dataset = GenDatasetWithIds(nb, dim, xb, ids);
        fiu_do_on("VecIndexImpl.BuildAll.throw_knowhere_exception", throw knowhere::KnowhereException(""));
        fiu_do_on("VecIndexImpl.BuildAll.throw_std_exception", throw std::exception());

        auto preprocessor = index_->BuildPreprocessor(dataset, cfg);
        index_->set_preprocessor(preprocessor);
        auto model = index_->Train(dataset, cfg);
        index_->set_index_model(model);
        index_->Add(dataset, cfg);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_UNEXPECTED_ERROR, e.what());
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_ERROR, e.what());
    }
    return Status::OK();
}

Status
VecIndexImpl::Add(const int64_t& nb, const float* xb, const int64_t* ids, const Config& cfg) {
    try {
        auto dataset = GenDatasetWithIds(nb, dim, xb, ids);
        fiu_do_on("VecIndexImpl.Add.throw_knowhere_exception", throw knowhere::KnowhereException(""));
        fiu_do_on("VecIndexImpl.Add.throw_std_exception", throw std::exception());
        index_->Add(dataset, cfg);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_UNEXPECTED_ERROR, e.what());
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_ERROR, e.what());
    }
    return Status::OK();
}

Status
VecIndexImpl::Search(const int64_t& nq, const float* xq, float* dist, int64_t* ids, const Config& cfg) {
    try {
        int64_t k = cfg[knowhere::meta::TOPK];
        auto dataset = GenDataset(nq, dim, xq);

        fiu_do_on("VecIndexImpl.Search.throw_knowhere_exception", throw knowhere::KnowhereException(""));
        fiu_do_on("VecIndexImpl.Search.throw_std_exception", throw std::exception());

        auto res = index_->Search(dataset, cfg);
        //{
        //    auto& ids = ids_array;
        //    auto& dists = dis_array;
        //    std::stringstream ss_id;
        //    std::stringstream ss_dist;
        //    for (auto i = 0; i < 10; i++) {
        //        for (auto j = 0; j < k; ++j) {
        //            ss_id << *(ids->data()->GetValues<int64_t>(1, i * k + j)) << " ";
        //            ss_dist << *(dists->data()->GetValues<float>(1, i * k + j)) << " ";
        //        }
        //        ss_id << std::endl;
        //        ss_dist << std::endl;
        //    }
        //    std::cout << "id\n" << ss_id.str() << std::endl;
        //    std::cout << "dist\n" << ss_dist.str() << std::endl;
        //}

        //        auto p_ids = ids_array->data()->GetValues<int64_t>(1, 0);
        //        auto p_dist = dis_array->data()->GetValues<float>(1, 0);

        // TODO(linxj): avoid copy here.
        auto res_ids = res->Get<int64_t*>(knowhere::meta::IDS);
        auto res_dist = res->Get<float*>(knowhere::meta::DISTANCE);
        memcpy(ids, res_ids, sizeof(int64_t) * nq * k);
        memcpy(dist, res_dist, sizeof(float) * nq * k);
        free(res_ids);
        free(res_dist);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_UNEXPECTED_ERROR, e.what());
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_ERROR, e.what());
    }
    return Status::OK();
}

knowhere::BinarySet
VecIndexImpl::Serialize() {
    type = ConvertToCpuIndexType(type);
    return index_->Serialize();
}

Status
VecIndexImpl::Load(const knowhere::BinarySet& index_binary) {
    index_->Load(index_binary);
    dim = Dimension();
    return Status::OK();
}

int64_t
VecIndexImpl::Dimension() {
    return index_->Dimension();
}

int64_t
VecIndexImpl::Count() {
    return index_->Count();
}

IndexType
VecIndexImpl::GetType() const {
    return type;
}

VecIndexPtr
VecIndexImpl::CopyToGpu(const int64_t& device_id, const Config& cfg) {
// TODO(linxj): exception handle
#ifdef MILVUS_GPU_VERSION
    auto gpu_index = knowhere::cloner::CopyCpuToGpu(index_, device_id, cfg);
    auto new_index = std::make_shared<VecIndexImpl>(gpu_index, ConvertToGpuIndexType(type));
    new_index->dim = dim;
    return new_index;
#else
    WRAPPER_LOG_ERROR << "Calling VecIndexImpl::CopyToGpu when we are using CPU version";
    throw WrapperException("Calling VecIndexImpl::CopyToGpu when we are using CPU version");
#endif
}

VecIndexPtr
VecIndexImpl::CopyToCpu(const Config& cfg) {
// TODO(linxj): exception handle
#ifdef MILVUS_GPU_VERSION
    auto cpu_index = knowhere::cloner::CopyGpuToCpu(index_, cfg);
    auto new_index = std::make_shared<VecIndexImpl>(cpu_index, ConvertToCpuIndexType(type));
    new_index->dim = dim;
    return new_index;
#else
    WRAPPER_LOG_ERROR << "Calling VecIndexImpl::CopyToCpu when we are using CPU version";
    throw WrapperException("Calling VecIndexImpl::CopyToCpu when we are using CPU version");
#endif
}

// VecIndexPtr
// VecIndexImpl::Clone() {
//    // TODO(linxj): exception handle
//    auto clone_index = std::make_shared<VecIndexImpl>(index_->Clone(), type);
//    clone_index->dim = dim;
//    return clone_index;
//}

int64_t
VecIndexImpl::GetDeviceId() {
#ifdef MILVUS_GPU_VERSION
    if (auto device_idx = std::dynamic_pointer_cast<knowhere::GPUIndex>(index_)) {
        return device_idx->GetGpuDevice();
    }
#else
    // else
    return -1;  // -1 == cpu
#endif
}

Status
VecIndexImpl::GetVectorById(const int64_t n, const int64_t* xid, float* x, const Config& cfg) {
    if (auto raw_index = std::dynamic_pointer_cast<knowhere::IVF>(index_)) {
    } else if (auto raw_index = std::dynamic_pointer_cast<knowhere::IDMAP>(index_)) {
    } else {
        throw WrapperException("not support");
    }

    try {
        auto dataset = std::make_shared<knowhere::Dataset>();
        dataset->Set(knowhere::meta::ROWS, n);
        dataset->Set(knowhere::meta::DIM, dim);
        dataset->Set(knowhere::meta::IDS, xid);

        auto res = index_->GetVectorById(dataset, cfg);

        // TODO(linxj): avoid copy here.
        auto res_x = res->Get<float*>(knowhere::meta::TENSOR);
        memcpy(x, res_x, sizeof(float) * n * dim);
        free(res_x);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_UNEXPECTED_ERROR, e.what());
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_ERROR, e.what());
    }
    return Status::OK();
}

Status
VecIndexImpl::SearchById(const int64_t& nq, const int64_t* xq, float* dist, int64_t* ids, const Config& cfg) {
    if (auto raw_index = std::dynamic_pointer_cast<knowhere::IVF>(index_)) {
    } else if (auto raw_index = std::dynamic_pointer_cast<knowhere::IDMAP>(index_)) {
    } else {
        throw WrapperException("not support");
    }

    try {
        int64_t k = cfg[knowhere::meta::TOPK];
        auto dataset = std::make_shared<knowhere::Dataset>();
        dataset->Set(knowhere::meta::ROWS, nq);
        dataset->Set(knowhere::meta::DIM, dim);
        dataset->Set(knowhere::meta::IDS, xq);

        auto res = index_->SearchById(dataset, cfg);
        //{
        //    auto& ids = ids_array;
        //    auto& dists = dis_array;
        //    std::stringstream ss_id;
        //    std::stringstream ss_dist;
        //    for (auto i = 0; i < 10; i++) {
        //        for (auto j = 0; j < k; ++j) {
        //            ss_id << *(ids->data()->GetValues<int64_t>(1, i * k + j)) << " ";
        //            ss_dist << *(dists->data()->GetValues<float>(1, i * k + j)) << " ";
        //        }
        //        ss_id << std::endl;
        //        ss_dist << std::endl;
        //    }
        //    std::cout << "id\n" << ss_id.str() << std::endl;
        //    std::cout << "dist\n" << ss_dist.str() << std::endl;
        //}

        //        auto p_ids = ids_array->data()->GetValues<int64_t>(1, 0);
        //        auto p_dist = dis_array->data()->GetValues<float>(1, 0);

        // TODO(linxj): avoid copy here.
        auto res_ids = res->Get<int64_t*>(knowhere::meta::IDS);
        auto res_dist = res->Get<float*>(knowhere::meta::DISTANCE);
        memcpy(ids, res_ids, sizeof(int64_t) * nq * k);
        memcpy(dist, res_dist, sizeof(float) * nq * k);
        free(res_ids);
        free(res_dist);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_UNEXPECTED_ERROR, e.what());
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_ERROR, e.what());
    }
    return Status::OK();
}

Status
VecIndexImpl::SetBlacklist(faiss::ConcurrentBitsetPtr list) {
    if (auto raw_index = std::dynamic_pointer_cast<knowhere::IVF>(index_)) {
        raw_index->SetBlacklist(list);
    } else if (auto raw_index = std::dynamic_pointer_cast<knowhere::IDMAP>(index_)) {
        raw_index->SetBlacklist(list);
    }
    return Status::OK();
}

Status
VecIndexImpl::GetBlacklist(faiss::ConcurrentBitsetPtr& list) {
    if (auto raw_index = std::dynamic_pointer_cast<knowhere::IVF>(index_)) {
        raw_index->GetBlacklist(list);
    } else if (auto raw_index = std::dynamic_pointer_cast<knowhere::IDMAP>(index_)) {
        raw_index->GetBlacklist(list);
    }
    return Status::OK();
}

Status
VecIndexImpl::SetUids(std::vector<segment::doc_id_t>& uids) {
    index_->SetUids(uids);
    return Status::OK();
}

const std::vector<segment::doc_id_t>&
VecIndexImpl::GetUids() const {
    return index_->GetUids();
}

const float*
BFIndex::GetRawVectors() {
    auto raw_index = std::dynamic_pointer_cast<knowhere::IDMAP>(index_);
    if (raw_index) {
        return raw_index->GetRawVectors();
    }
    return nullptr;
}

const int64_t*
BFIndex::GetRawIds() {
    return std::static_pointer_cast<knowhere::IDMAP>(index_)->GetRawIds();
}

ErrorCode
BFIndex::Build(const Config& cfg) {
    try {
        fiu_do_on("BFIndex.Build.throw_knowhere_exception", throw knowhere::KnowhereException(""));
        fiu_do_on("BFIndex.Build.throw_std_exception", throw std::exception());
        dim = cfg[knowhere::meta::DIM];
        std::static_pointer_cast<knowhere::IDMAP>(index_)->Train(cfg);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return KNOWHERE_UNEXPECTED_ERROR;
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return KNOWHERE_ERROR;
    }
    return KNOWHERE_SUCCESS;
}

Status
BFIndex::BuildAll(const int64_t& nb, const float* xb, const int64_t* ids, const Config& cfg, const int64_t& nt,
                  const float* xt) {
    try {
        dim = cfg[knowhere::meta::DIM];
        auto dataset = GenDatasetWithIds(nb, dim, xb, ids);
        fiu_do_on("BFIndex.BuildAll.throw_knowhere_exception", throw knowhere::KnowhereException(""));
        fiu_do_on("BFIndex.BuildAll.throw_std_exception", throw std::exception());

        std::static_pointer_cast<knowhere::IDMAP>(index_)->Train(cfg);
        index_->Add(dataset, cfg);
    } catch (knowhere::KnowhereException& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_UNEXPECTED_ERROR, e.what());
    } catch (std::exception& e) {
        WRAPPER_LOG_ERROR << e.what();
        return Status(KNOWHERE_ERROR, e.what());
    }
    return Status::OK();
}

Status
BFIndex::AddWithoutIds(const int64_t& nb, const float* xb, const Config& cfg) {
    auto ret_ds = std::make_shared<knowhere::Dataset>();
    ret_ds->Set(knowhere::meta::ROWS, nb);
    ret_ds->Set(knowhere::meta::TENSOR, xb);
    std::static_pointer_cast<knowhere::IDMAP>(index_)->AddWithoutId(ret_ds, cfg);
    return Status::OK();
}

}  // namespace engine
}  // namespace milvus
