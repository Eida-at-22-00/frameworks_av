/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <new>

#include <android-base/no_destructor.h>
#include <apex/ApexCodecs.h>

// TODO: remove when we have real implementations
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"

struct ApexCodec_ComponentStore {
    ApexCodec_ComponentStore() = default;
};

ApexCodec_ComponentStore *ApexCodec_GetComponentStore() {
    ::android::base::NoDestructor<ApexCodec_ComponentStore> store;
    return store.get();
}

ApexCodec_ComponentTraits *ApexCodec_Traits_get(
        ApexCodec_ComponentStore *store, size_t index) {
    return nullptr;
}

ApexCodec_Status ApexCodec_Component_create(
        ApexCodec_ComponentStore *store, const char *name, ApexCodec_Component **comp) {
    *comp = nullptr;
    return APEXCODEC_STATUS_NOT_FOUND;
}

void ApexCodec_Component_destroy(ApexCodec_Component *comp) {}

ApexCodec_Status ApexCodec_Component_start(ApexCodec_Component *comp) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_Component_flush(ApexCodec_Component *comp) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_Component_reset(ApexCodec_Component *comp) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Configurable *ApexCodec_Component_getConfigurable(
        ApexCodec_Component *comp) {
    return nullptr;
}

ApexCodec_Status ApexCodec_SupportedValues_getTypeAndValues(
        ApexCodec_SupportedValues *supportedValues,
        ApexCodec_SupportedValuesType *type,
        ApexCodec_SupportedValuesNumberType *numberType,
        ApexCodec_Value **values,
        uint32_t *numValues) {
    return APEXCODEC_STATUS_OMITTED;
}

void ApexCodec_SupportedValues_release(ApexCodec_SupportedValues *values) {}

ApexCodec_Status ApexCodec_SettingResults_getResultAtIndex(
        ApexCodec_SettingResults *results,
        size_t index,
        ApexCodec_SettingResultFailure *failure,
        ApexCodec_ParamFieldValues *field,
        ApexCodec_ParamFieldValues **conflicts,
        size_t *numConflicts) {
    return APEXCODEC_STATUS_OMITTED;
}

void ApexCodec_SettingResults_release(ApexCodec_SettingResults *results) {}

ApexCodec_Status ApexCodec_Component_process(
        ApexCodec_Component *comp,
        const ApexCodec_Buffer *input,
        ApexCodec_Buffer *output,
        size_t *consumed,
        size_t *produced) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_Configurable_config(
        ApexCodec_Configurable *comp,
        ApexCodec_LinearBuffer *config,
        ApexCodec_SettingResults **results) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_Configurable_query(
        ApexCodec_Configurable *comp,
        uint32_t indices[],
        size_t numIndices,
        ApexCodec_LinearBuffer *config,
        size_t *written) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_ParamDescriptors_getIndices(
        ApexCodec_ParamDescriptors *descriptors,
        uint32_t **indices,
        size_t *numIndices) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_ParamDescriptors_getDescriptor(
        ApexCodec_ParamDescriptors *descriptors,
        uint32_t index,
        ApexCodec_ParamAttribute *attr,
        const char **name,
        uint32_t **dependencies,
        size_t *numDependencies) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_ParamDescriptors_release(
        ApexCodec_ParamDescriptors *descriptors) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_Configurable_querySupportedParams(
        ApexCodec_Configurable *comp,
        ApexCodec_ParamDescriptors **descriptors) {
    return APEXCODEC_STATUS_OMITTED;
}

ApexCodec_Status ApexCodec_Configurable_querySupportedValues(
        ApexCodec_Configurable *comp,
        ApexCodec_SupportedValuesQuery *queries,
        size_t numQueries) {
    return APEXCODEC_STATUS_OMITTED;
}

#pragma clang diagnostic pop