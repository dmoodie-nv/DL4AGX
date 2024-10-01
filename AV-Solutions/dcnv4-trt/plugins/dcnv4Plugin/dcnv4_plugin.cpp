/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DEBUG
#define DEBUG 0 // set debug mode, if you want to see the api call, set it to 1
#endif

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include "./dcnv4_plugin.h"

using namespace nvinfer1;
using nvinfer1::plugin::DCNv4_Plugin;
using nvinfer1::plugin::DCNv4_PluginChecker;

namespace
{
static const char* DCNV4_PLUGIN_VERSION{"1"};
static const char* DCNV4_PLUGIN_NAME{"DCNv4_Plugin"};
} // namespace

// Static class fields initialization
PluginFieldCollection DCNv4_PluginChecker::mFC{};
std::vector<PluginField> DCNv4_PluginChecker::mPluginAttributes{};

// Implementation of plugin class
DCNv4_Plugin::DCNv4_Plugin(PluginFieldCollection const& fc) noexcept {    
    (void) fc;
    const PluginField* fields = fc.fields;
    for (int i=0; i<fc.nbFields; i++) {
        auto curr_field = fields[i];
        if( strcmp(curr_field.name, "kh") == 0 ) {
            kh = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "kw") == 0 ) {
            kw = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "sh") == 0 ) {
            sh = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "sw") == 0 ) {
            sw = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "ph") == 0 ) {
            ph = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "pw") == 0 ) {
            pw = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "dh") == 0 ) {
            dh = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "dw") == 0 ) {
            dw = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "group") == 0 ) {
            group = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "group_channels") == 0 ) {
            group_channels = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "offscale") == 0 ) {
            offscale = reinterpret_cast<const float*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "step") == 0 ) {
            step = reinterpret_cast<const int*>(curr_field.data)[0];
        } else if( strcmp(curr_field.name, "remove_center") == 0 ) {
            remove_center = reinterpret_cast<const int*>(curr_field.data)[0];
        } else {
            throw std::runtime_error("bad field");
        }
    }
    print_log("ctor, %d %d", kh, kw);
    print_log("group=%d group_channels=%d", group, group_channels);

#ifdef USE_PTX
    elf = getElf();
    elf_buf = elf->elf;
    elf_size = elf->elf_size;
    print_log("elf_size=%d\n", elf_size);

    for( int s=0; s<4; s++ ) { elf_v2.Compile(s); }
#endif
}

DCNv4_Plugin::DCNv4_Plugin(const std::string name, const void* data, size_t length)
    :mName(name)
{
    print_log("Constructor from serial data");
    const char* d = reinterpret_cast<const char*>(data);
    const char* a = d;
    kh = read<int>(d); kw = read<int>(d);
    sh = read<int>(d); sw = read<int>(d);
    ph = read<int>(d); pw = read<int>(d);
    dh = read<int>(d); dw = read<int>(d);
    group = read<int>(d); group_channels = read<int>(d);
    offscale = read<float>(d);
    step = read<int>(d); remove_center = read<int>(d);
    mDataType = read<nvinfer1::DataType>(d);
    mInputDims = read<nvinfer1::Dims>(d);
    mOutputDims = read<nvinfer1::Dims>(d);
    padded_offset_dim = read<int>(d);

#ifdef USE_PTX
    // n_elf is fixed to 1
    int n_elf = read<int>(d);
    int elf_size_ = read<int>(d);
    printf("elf_size_ in engine = %d\n", elf_size_);
    elf_buf = new char[elf_size_];
    elf_size = elf_size_;
    std::memcpy(elf_buf, d, elf_size_);
    elf = new DCNv4Elf(elf_buf, elf_size_);
    d += elf_size_;

    for( int s=0; s<4; s++ ) {
        size_t elf_size_ = read<size_t>(d);
        printf("stage %d, elf_size_ in engine = %d\n", s, elf_size_);
        elf_v2.Setup(s, d, elf_size_);
        d += elf_size_;
    }
#endif
}

int DCNv4_Plugin::getNbOutputs() const noexcept {
    print_log("Get number of outputs");
    return 1;
}

Dims DCNv4_Plugin::getOutputDimensions(
    int index, const Dims* inputs, int nbInputDims
) noexcept {
    print_log("Get output dimensions");

    assert(index == 0 && nbInputDims == 2);

    const int batch = 1; // inputs[0].d[0];
    const int height_in = inputs[0].d[0];
    const int width_in = inputs[0].d[1];
    const int channel_in = inputs[0].d[2];

    const int height_out =
        (height_in + 2 * ph - (dh * (kh - 1) + 1)) / sh + 1;
    const int width_out =
        (width_in + 2 * pw - (dw * (kw - 1) + 1)) / sw + 1;

    Dims ret;
    ret.nbDims = 3;

    ret.d[0] = height_out;
    ret.d[1] = width_out;
    ret.d[2] = group * group_channels;

    print_log("nbInputDims=%d,index=%d,out=[%d,%d,%d]", 
        nbInputDims, index, 
        height_out, width_out, group * group_channels);
    return ret;
}

int DCNv4_Plugin::initialize() noexcept {
    size_t stackSizeLimit = 0;    
    cudaDeviceGetLimit(&stackSizeLimit, cudaLimitStackSize);
    return 0;
}

void DCNv4_Plugin::terminate() noexcept {}

size_t DCNv4_Plugin::getWorkspaceSize(int maxBatchSize) const noexcept {
    return 0;
}

size_t DCNv4_Plugin::getSerializationSize() const noexcept {
    // Calculate the serialization size required for your plugin's data
    size_t serializationSize = 0;
    serializationSize += sizeof(kh); serializationSize += sizeof(kw);
    serializationSize += sizeof(sh); serializationSize += sizeof(sw);
    serializationSize += sizeof(ph); serializationSize += sizeof(pw);
    serializationSize += sizeof(dh); serializationSize += sizeof(dw);
    serializationSize += sizeof(group); serializationSize += sizeof(group_channels);
    serializationSize += sizeof(offscale); 
    serializationSize += sizeof(step); serializationSize += sizeof(remove_center);
    serializationSize += sizeof(static_cast<int>(mDataType));
    serializationSize += sizeof(nvinfer1::Dims);
    serializationSize += sizeof(nvinfer1::Dims);
    serializationSize += sizeof(padded_offset_dim);

#ifdef USE_PTX
    serializationSize += sizeof(int); // n_elf

    serializationSize += sizeof(int); // elf_size
    serializationSize += elf_size; // elf_buf

    for( int s=0; s<4; s++ ) {
        size_t was_size = serializationSize;
        serializationSize += sizeof(size_t);
        serializationSize += elf_v2.kernels[s].mElfSize;
        print_log("getSerializationSize, stage %d, was %d, size %d", s, was_size, elf_v2.kernels[s].mElfSize);
    }
#endif
    print_log("%d", serializationSize);
    return serializationSize;
}

void DCNv4_Plugin::serialize(void* buffer) const noexcept {
    print_log("Serialize DCNv4_Plugin");
    char* d = reinterpret_cast<char*>(buffer);
    const char* a = d;
    write(d, kh); write(d, kw);
    write(d, sh); write(d, sw);
    write(d, ph); write(d, pw);
    write(d, dh); write(d, dw);
    write(d, group); write(d, group_channels);
    write(d, offscale);
    write(d, step); write(d, remove_center);
    write(d, mDataType);
    write(d, mInputDims);
    write(d, mOutputDims);
    write(d, padded_offset_dim);

#ifdef USE_PTX
    write(d, 1);

    write(d, elf_size);
    std::memcpy(d, elf_buf, elf_size);
    d += elf_size;

    for( int s=0; s<4; s++ ) {
        const DCNv4Kernel& k = elf_v2.kernels[s];
        write(d, k.mElfSize);
        print_log("%d", k.mElfSize);
        std::memcpy(d, k.mElf, k.mElfSize);
        d += k.mElfSize;
    }
#endif
}

void DCNv4_Plugin::configurePlugin(
    PluginTensorDesc const* in, int32_t nbInput, 
    PluginTensorDesc const* out, int32_t nbOutput
) noexcept {
    print_log("DCNv4_Plugin configure plugin");
    mDataType = in[0].type;
    mInputDims = in[0].dims;
    mOutputDims = out[0].dims;
    padded_offset_dim = in[1].dims.d[2];
}

bool DCNv4_Plugin::supportsFormatCombination(
    int pos, const PluginTensorDesc* inOut, int nbInputs, int nbOutputs
) const noexcept {
    bool f1 = inOut[pos].format == TensorFormat::kLINEAR;
    bool f2 = inOut[pos].type == DataType::kHALF || inOut[pos].type == DataType::kFLOAT;
    bool f3 = inOut[pos].type == inOut[0].type;
    return f1 && f2 && f3;
}

DataType DCNv4_Plugin::getOutputDataType(
    int index, const DataType* inputTypes, int nbInputs
) const noexcept {
    return inputTypes[0];
}

const char* DCNv4_Plugin::getPluginType() const noexcept {
    return DCNV4_PLUGIN_NAME;
}

const char* DCNv4_Plugin::getPluginVersion() const noexcept {
    return DCNV4_PLUGIN_VERSION;
}

void DCNv4_Plugin::destroy() noexcept {
    delete this;
}

IPluginV2IOExt* DCNv4_Plugin::clone() const noexcept {
    print_log("clone");
    auto* plugin = new DCNv4_Plugin(*this);
    plugin->setPluginNamespace(mPluginNamespace);
    return plugin;
}

void DCNv4_Plugin::setPluginNamespace(const char* pluginNamespace) noexcept {
    mPluginNamespace = pluginNamespace;
}

const char* DCNv4_Plugin::getPluginNamespace() const noexcept {
    return mPluginNamespace;
}

bool DCNv4_Plugin::isOutputBroadcastAcrossBatch(
    int outputIndex, const bool* inputIsBroadcasted, int nbInputs
) const noexcept {
    return false;
}

bool DCNv4_Plugin::canBroadcastInputAcrossBatch(int inputIndex) const noexcept {
    return false;
}

// Implementation of plugin checker
DCNv4_PluginChecker::DCNv4_PluginChecker() {
    setupPluginAttributes(mPluginAttributes);
    mFC.nbFields = (size_t)(mPluginAttributes.size());
    mFC.fields = mPluginAttributes.data();
}

void DCNv4_PluginChecker::setupPluginAttributes(std::vector<PluginField>& attributes) {
    attributes.clear();
    attributes.emplace_back(PluginField("kh", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("kw", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("sh", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("sw", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("ph", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("pw", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("dh", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("dw", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("group", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("group_channels", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("offscale", nullptr, PluginFieldType::kFLOAT32, 1));
    attributes.emplace_back(PluginField("step", nullptr, PluginFieldType::kINT32, 1));
    attributes.emplace_back(PluginField("remove_center", nullptr, PluginFieldType::kINT32, 1));
}

bool DCNv4_PluginChecker::validate(
    char const *name, void const *serialData, size_t serialLength, 
    nvinfer1::PluginTensorDesc const *in, size_t nbInputs, 
    nvinfer1::PluginTensorDesc const *out, size_t nbOutputs, 
    int64_t workspaceSize
) const noexcept {
    print_log("validate");
    // Custom logic can be written here to validate the UnaryPlugin.
    bool valid = true;
    bool const validNbInputsAndOutputs = (nbOutputs == 1) && (nbInputs == 2);
    valid &= validNbInputsAndOutputs;
    if (!valid) {
        return false;
    }
    bool const validDataType1 = (in[1].type == DataType::kHALF) && (out->type == DataType::kHALF);
    bool const validDataType2 = (in[1].type == DataType::kFLOAT) && (out->type == DataType::kFLOAT);
    bool const validDataType3 = (in[1].type == DataType::kINT8) && (out->type == DataType::kINT8);
    bool const validDataType = validDataType1 || validDataType2 || validDataType3;
    valid &= validDataType;
    return valid;
}

const char* DCNv4_PluginChecker::getPluginName(
) const noexcept {
    return DCNV4_PLUGIN_NAME;
}

const char* DCNv4_PluginChecker::getPluginVersion(
) const noexcept {
    return DCNV4_PLUGIN_VERSION;
}

void DCNv4_PluginChecker::setPluginNamespace(
    const char* pluginNamespace
) noexcept {
    mNamespace = pluginNamespace;
}

const char* DCNv4_PluginChecker::getPluginNamespace(
) const noexcept {
    return mNamespace.c_str();
}

const PluginFieldCollection* DCNv4_PluginChecker::getFieldNames(
) noexcept {
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
    return &mFC;
}

IPluginV2IOExt* DCNv4_PluginChecker::createPlugin(
    const char* name, const PluginFieldCollection* fc
) noexcept {
    auto plugin = new DCNv4_Plugin(*fc);
    plugin->setPluginNamespace(mNamespace.c_str());
    mFC = *fc;
    return plugin;
}

IPluginV2IOExt* DCNv4_PluginChecker::deserializePlugin(
    const char* name, const void* serialData, size_t serialLength
) noexcept {
    return new DCNv4_Plugin(name, serialData, serialLength);
}