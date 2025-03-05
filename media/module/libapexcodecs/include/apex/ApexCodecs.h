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

#pragma once

#include <sys/cdefs.h>
#include <errno.h>
#include <stdint.h>

#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/versioning.h>

__BEGIN_DECLS

/**
 * An API to access and operate codecs implemented within an APEX module,
 * used only by the OS when using the codecs within a client process
 * (instead of via a HAL).
 *
 * NOTE: Many of the constants and types mirror the ones in the Codec 2.0 API.
 */

/**
 * Error code for ApexCodec APIs.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_Status : int32_t {
    APEXCODEC_STATUS_OK        = 0,

    /* bad input */
    APEXCODEC_STATUS_BAD_VALUE = EINVAL,
    APEXCODEC_STATUS_BAD_INDEX = ENXIO,
    APEXCODEC_STATUS_CANNOT_DO = ENOTSUP,

    /* bad sequencing of events */
    APEXCODEC_STATUS_DUPLICATE = EEXIST,
    APEXCODEC_STATUS_NOT_FOUND = ENOENT,
    APEXCODEC_STATUS_BAD_STATE = EPERM,
    APEXCODEC_STATUS_BLOCKING  = EWOULDBLOCK,
    APEXCODEC_STATUS_CANCELED  = EINTR,

    /* bad environment */
    APEXCODEC_STATUS_NO_MEMORY = ENOMEM,
    APEXCODEC_STATUS_REFUSED   = EACCES,

    APEXCODEC_STATUS_TIMED_OUT = ETIMEDOUT,

    /* bad versioning */
    APEXCODEC_STATUS_OMITTED   = ENOSYS,

    /* unknown fatal */
    APEXCODEC_STATUS_CORRUPTED = EFAULT,
    APEXCODEC_STATUS_NO_INIT   = ENODEV,
} ApexCodec_Status;

/**
 * Enum that represents the kind of component
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_Kind : uint32_t {
    /**
     * The component is of a kind that is not listed below.
     */
    APEXCODEC_KIND_OTHER = 0x0,
    /**
     * The component is a decoder, which decodes coded bitstream
     * into raw buffers.
     *
     * Introduced in API 36.
     */
    APEXCODEC_KIND_DECODER = 0x1,
    /**
     * The component is an encoder, which encodes raw buffers
     * into coded bitstream.
     *
     * Introduced in API 36.
     */
    APEXCODEC_KIND_ENCODER = 0x2,
} ApexCodec_Kind;

typedef enum ApexCodec_Domain : uint32_t {
    /**
     * A component domain that is not listed below.
     *
     * Introduced in API 36.
     */
    APEXCODEC_DOMAIN_OTHER = 0x0,
    /**
     * A component domain that operates on video.
     *
     * Introduced in API 36.
     */
    APEXCODEC_DOMAIN_VIDEO = 0x1,
    /**
     * A component domain that operates on audio.
     *
     * Introduced in API 36.
     */
    APEXCODEC_DOMAIN_AUDIO = 0x2,
    /**
     * A component domain that operates on image.
     *
     * Introduced in API 36.
     */
    APEXCODEC_DOMAIN_IMAGE = 0x3,
} ApexCodec_Domain;

/**
 * Handle for component traits such as name, media type, kind (decoder/encoder),
 * domain (audio/video/image), etc.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_ComponentTraits {
    /**
     * The name of the component.
     */
    const char *name;
    /**
     * The supported media type of the component.
     */
    const char *mediaType;
    /**
     * The kind of the component.
     */
    ApexCodec_Kind kind;
    /**
     * The domain on which the component operates.
     */
    ApexCodec_Domain domain;
} ApexCodec_ComponentTraits;

/**
 * An opaque struct that represents a component store.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_ComponentStore ApexCodec_ComponentStore;

/**
 * Get the component store object. This function never fails.
 *
 * \return component store object.
 */
ApexCodec_ComponentStore *ApexCodec_GetComponentStore()
        __INTRODUCED_IN(36);

/**
 * Get the traits object of a component at given index. ApexCodecs_Traits_*
 * functions are used to extract information from the traits object.
 *
 * Returns nullptr if index is out of bounds. The returned object is owned by
 * ApexCodec_ComponentStore object and the client should not delete it.
 *
 * The client can iterate through the traits objects by calling this function
 * with an index incrementing from 0 until it gets a nullptr.
 *
 * \param index index of the traits object to query
 * \return traits object at the index, or nullptr if the index is out of bounds.
 */
ApexCodec_ComponentTraits *ApexCodec_Traits_get(
        ApexCodec_ComponentStore *store, size_t index) __INTRODUCED_IN(36);

/**
 * An opaque struct that represents a codec.
 */
typedef struct ApexCodec_Component ApexCodec_Component;

/**
 * Create a component by the name.
 *
 * \param store the component store
 * \param name the name of the component
 * \param component out-param to be filled with the component; must not be null
 * \return  APEXCODEC_STATUS_OK         if successful
 *          APEXCODEC_STATUS_NOT_FOUND  if the name is not found
 */
ApexCodec_Status ApexCodec_Component_create(
        ApexCodec_ComponentStore *store, const char *name, ApexCodec_Component **comp)
        __INTRODUCED_IN(36);

/**
 * Destroy the component by the handle. It is invalid to call component methods on the handle
 * after calling this method. It is no-op to call this method with |comp| == nullptr.
 *
 * \param comp the handle for the component
 */
void ApexCodec_Component_destroy(ApexCodec_Component *comp) __INTRODUCED_IN(36);

/**
 * Start the component. The component is ready to process buffers after this call.
 *
 * \param comp the handle for the component
 */
ApexCodec_Status ApexCodec_Component_start(
        ApexCodec_Component *comp) __INTRODUCED_IN(36);

/**
 * Flush the component's internal states. This operation preserves the existing configurations.
 *
 * \param comp the handle for the component
 */
ApexCodec_Status ApexCodec_Component_flush(
        ApexCodec_Component *comp) __INTRODUCED_IN(36);

/**
 * Resets the component to the initial state, right after creation. Note that the configuration
 * will also revert to the initial state, so if there are configurations required those should be
 * set again to use the component.
 *
 * \param comp the handle for the component
 */
ApexCodec_Status ApexCodec_Component_reset(
        ApexCodec_Component *comp) __INTRODUCED_IN(36);

/**
 * An opaque struct that represents a configurable part of the component.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_Configurable ApexCodec_Configurable;

/**
 * Return the configurable object for the given ApexCodec_Component.
 * The returned object has the same lifecycle as |comp|.
 *
 * \param comp the handle for the component
 * \return the configurable object handle
 */
ApexCodec_Configurable *ApexCodec_Component_getConfigurable(
        ApexCodec_Component *comp) __INTRODUCED_IN(36);

/**
 * Enum that represents the flags for ApexCodec_Buffer.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_BufferFlags : uint32_t {
    APEXCODEC_FLAG_DROP_FRAME    = (1 << 0),
    APEXCODEC_FLAG_END_OF_STREAM = (1 << 1),
    APEXCODEC_FLAG_DISCARD_FRAME = (1 << 2),
    APEXCODEC_FLAG_INCOMPLETE    = (1 << 3),
    APEXCODEC_FLAG_CORRECTED     = (1 << 4),
    APEXCODEC_FLAG_CORRUPT       = (1 << 5),
    APEXCODEC_FLAG_CODEC_CONFIG  = (1u << 31),
} ApexCodec_BufferFlags;

/**
 * Enum that represents the type of buffer.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_BufferType : uint32_t {
    APEXCODEC_BUFFER_TYPE_INVALID,
    APEXCODEC_BUFFER_TYPE_LINEAR,
    APEXCODEC_BUFFER_TYPE_LINEAR_CHUNKS,
    APEXCODEC_BUFFER_TYPE_GRAPHIC,
    APEXCODEC_BUFFER_TYPE_GRAPHIC_CHUNKS,
} ApexCodec_BufferType;

/**
 * Struct that represents the memory for ApexCodec_Buffer.
 *
 * All memory regions have the simple 1D representation.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_LinearBuffer {
    /**
     * A pointer to the start of the buffer. This is not aligned.
     */
    uint8_t *data;
    /**
     * Size of the buffer. The memory region between |data| (inclusive) and
     * |data + size| (exclusive) is assumed to be valid for read/write.
     */
    size_t size;
} ApexCodec_LinearBuffer;

/**
 * Struct that represents a buffer for ApexCodec_Component.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_Buffer {
    /**
     * Flags associated with the buffer.
     */
    ApexCodec_BufferFlags flags;
    /**
     * For input buffers client assign a unique sequential index for each buffer. For output buffers
     * it is the same as the associated input buffer's frame index.
     */
    uint64_t frameIndex;
    /**
     * A timestamp associated with the buffer in microseconds.
     */
    uint64_t timestampUs;
    /**
     * The type of the buffer. The component may reject request to process a buffer with the wrong
     * type. For example, a video decoder will reject an input buffer with type BUFFER_TYPE_GRAPHIC,
     * or an output buffer with type BUFFER_TYPE_LINEAR.
     */
    ApexCodec_BufferType type;
    /**
     * The actual memory for the buffer.
     */
    union {
        ApexCodec_LinearBuffer linear;
        AHardwareBuffer *graphic;
    } memory;
    /**
     * Config updates associated with the buffer. For input buffers these are sent to the component
     * at the specific input frame. For output buffers these are config updates as a result of
     * processing the buffer.
     */
    ApexCodec_LinearBuffer configUpdates;
} ApexCodec_Buffer;

/**
 * Enum that represents the query type for the supported values.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_SupportedValuesQueryType : uint32_t {
    /** Query all possible supported values regardless of current configuration */
    APEXCODEC_SUPPORTED_VALUES_QUERY_POSSIBLE,
    /** Query supported values at current configuration */
    APEXCODEC_SUPPORTED_VALUES_QUERY_CURRENT,
} ApexCodec_SupportedValuesQueryType;

/**
 * Enum that represents the type of the supported values.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_SupportedValuesType : uint32_t {
    /** The supported values are empty. */
    APEXCODEC_SUPPORTED_VALUES_EMPTY,
    /**
     * The supported values are represented by a range defined with {min, max, step, num, den}.
     *
     * If step is 0 and num and denom are both 1, the supported values are any value, for which
     * min <= value <= max.
     *
     * Otherwise, the range represents a geometric/arithmetic/multiply-accumulate series, where
     * successive supported values can be derived from previous values (starting at min), using the
     * following formula:
     *  v[0] = min
     *  v[i] = v[i-1] * num / denom + step for i >= 1, while min < v[i] <= max.
     */
    APEXCODEC_SUPPORTED_VALUES_RANGE,
    /** The supported values are represented by a list of values. */
    APEXCODEC_SUPPORTED_VALUES_VALUES,
    /** The supported values are represented by a list of flags. */
    APEXCODEC_SUPPORTED_VALUES_FLAGS,
} ApexCodec_SupportedValuesType;

/**
 * Enum that represents numeric types of the supported values.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_SupportedValuesNumberType : uint32_t {
    APEXCODEC_SUPPORTED_VALUES_TYPE_NONE   = 0,
    APEXCODEC_SUPPORTED_VALUES_TYPE_INT32  = 1,
    APEXCODEC_SUPPORTED_VALUES_TYPE_UINT32 = 2,
    // RESERVED                            = 3,
    APEXCODEC_SUPPORTED_VALUES_TYPE_INT64  = 4,
    APEXCODEC_SUPPORTED_VALUES_TYPE_UINT64 = 5,
    // RESERVED                            = 6,
    APEXCODEC_SUPPORTED_VALUES_TYPE_FLOAT  = 7,
} ApexCodec_SupportedValuesNumberType;

/**
 * Union of primitive types.
 *
 * Introduced in API 36.
 */
typedef union {
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    float f;
} ApexCodec_Value;

/**
 * An opaque struct that represents the supported values of a parameter.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_SupportedValues ApexCodec_SupportedValues;

/**
 * Extract information from ApexCodec_SupportedValues object.
 *
 * \param [in] supportedValues the supported values object
 * \param [out] type        pointer to be filled with the type of the supported values
 * \param [out] numberType  pointer to be filled with the numeric type of the supported values
 * \param [out] values      pointer to be filled with the array of the actual supported values.
 *                          if type == APEXCODEC_SUPPORTED_VALUES_EMPTY: nullptr
 *                          if type == APEXCODEC_SUPPORTED_VALUES_RANGE: {min, max, step, num, den}
 *                          if type == APEXCODEC_SUPPORTED_VALUES_VALUES/_FLAGS:
 *                              the array of supported values/flags
 *                          the array is owned by the |supportedValues| object and the client
 *                          should not free it.
 * \param [out] numValues   pointer to be filled with the number of values.
 *                          if type == APEXCODEC_SUPPORTED_VALUES_EMPTY: 0
 *                          if type == APEXCODEC_SUPPORTED_VALUES_RANGE: 5
 *                          if type == APEXCODEC_SUPPORTED_VALUES_VALUES/_FLAGS: varies
 */
ApexCodec_Status ApexCodec_SupportedValues_getTypeAndValues(
        ApexCodec_SupportedValues *supportedValues,
        ApexCodec_SupportedValuesType *type,
        ApexCodec_SupportedValuesNumberType *numberType,
        ApexCodec_Value **values,
        uint32_t *numValues) __INTRODUCED_IN(36);

/**
 * Release the supported values object.
 *
 * \param values the supported values object
 */
void ApexCodec_SupportedValues_release(
        ApexCodec_SupportedValues *values) __INTRODUCED_IN(36);

/**
 * Struct that represents the result of ApexCodec_Configurable_config.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_SettingResults ApexCodec_SettingResults;

/**
 * Enum that represents the failure code of ApexCodec_SettingResults.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_SettingResultFailure : uint32_t {
    /** parameter type is not supported */
    APEXCODEC_SETTING_RESULT_BAD_TYPE,
    /** parameter is not supported on the specific port */
    APEXCODEC_SETTING_RESULT_BAD_PORT,
    /** parameter is not supported on the specific stream */
    APEXCODEC_SETTING_RESULT_BAD_INDEX,
    /** parameter is read-only */
    APEXCODEC_SETTING_RESULT_READ_ONLY,
    /** parameter mismatches input data */
    APEXCODEC_SETTING_RESULT_MISMATCH,
    /** strict parameter does not accept value for the field at all */
    APEXCODEC_SETTING_RESULT_BAD_VALUE,
    /** strict parameter field value conflicts with another settings */
    APEXCODEC_SETTING_RESULT_CONFLICT,
    /** strict parameter field is out of range due to other settings */
    APEXCODEC_SETTING_RESULT_UNSUPPORTED,
    /**
     * field does not accept the requested parameter value at all. It has been corrected to
     * the closest supported value. This failure mode is provided to give guidance as to what
     * are the currently supported values for this field (which may be a subset of the at-all-
     * potential values)
     */
    APEXCODEC_SETTING_RESULT_INFO_BAD_VALUE,
    /**
     * requested parameter value is in conflict with an/other setting(s)
     * and has been corrected to the closest supported value. This failure
     * mode is given to provide guidance as to what are the currently supported values as well
     * as to optionally provide suggestion to the client as to how to enable the requested
     * parameter value.
     */
    APEXCODEC_SETTING_RESULT_INFO_CONFLICT,
} ApexCodec_SettingResultFailure;

/**
 * Struct that represents a field and its supported values of a parameter.
 *
 * The offset and size of the field are where the field is located in the blob representation of
 * the parameter, as used in the ApexCodec_Configurable_query() and ApexCodec_Configurable_config(),
 * for example.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_ParamFieldValues {
    /** index of the param */
    uint32_t index;
    /** offset of the param field */
    uint32_t offset;
    /** size of the param field */
    uint32_t size;
    /** currently supported values of the param field */
    ApexCodec_SupportedValues *values;
} ApexCodec_ParamFieldValues;

/**
 * Extract the result of ApexCodec_Configurable_config.
 * The client can iterate through the results with index starting from 0 until this function returns
 * APEXCODEC_STATUS_NOT_FOUND.
 *
 * \param [in]  result  the result object
 * \param [in]  index   the index of the result to extract, starts from 0.
 * \param [out] failure pointer to be filled with the failure code
 * \param [out] field   pointer to be filled with the field that failed.
 *                      |field->value| is owned by the |result| object and the client should not
 *                      free it.
 * \param [out] conflicts   pointer to be filled with the array of conflicts.
 *                          nullptr if |numConflicts| is 0.
 *                          the array and its content is owned by the |result| object and the client
 *                          should not free it.
 * \param [out] numConflicts pointer to be filled with the number of conflicts
 *                          may be 0 if there are no conflicts
 * \return APEXCODEC_STATUS_OK         if successful
 * \return APEXCODEC_STATUS_NOT_FOUND  if index is out of range
 */
ApexCodec_Status ApexCodec_SettingResults_getResultAtIndex(
        ApexCodec_SettingResults *results,
        size_t index,
        ApexCodec_SettingResultFailure *failure,
        ApexCodec_ParamFieldValues *field,
        ApexCodec_ParamFieldValues **conflicts,
        size_t *numConflicts) __INTRODUCED_IN(36);

/**
 * Release the setting result object.
 *
 * \param result the setting result object
 */
void ApexCodec_SettingResults_release(
        ApexCodec_SettingResults *results) __INTRODUCED_IN(36);

/**
 * Process one frame from |input|, and produce one frame to |output| if possible.
 * When successfully filled, |output->memory.linear| has the size adjusted to the produced
 * output size, in case of linear buffers. |input->configUpdates| is applied with the input
 * buffer; |output->configUpdates| contains config updates as a result of processing the frame.
 *
 * \param comp      the component to process the buffers
 * \param input     the input buffer; when nullptr, the component should fill |output| if there are
 *                  any pending output buffers.
 * \param output    the output buffer, should not be nullptr.
 * \param consumed  the number of consumed bytes from the input buffer
 *                  set to 0 if no input buffer has been consumed, including |input| is nullptr.
 *                  for graphic buffers, any non-zero value means that the input buffer is consumed.
 * \param produced  the number of bytes produced on the output buffer
 *                  set to 0 if no output buffer has been produced.
 *                  for graphic buffers, any non-zero value means that the output buffer is filled.
 * \return APEXCODEC_STATUS_OK         if successful
 * \return APEXCODEC_STATUS_NO_MEMORY  if the output buffer is not suitable to hold the output frame
 *                                     the client should retry with a new output buffer;
 *                                     configUpdates should have the information to update
 *                                     the buffer size.
 * \return APEXCODEC_STATUS_BAD_VALUE  if the parameters are bad
 * \return APEXCODEC_STATUS_BAD_STATE  if the component is not in the right state
 *                                     to process the frame
 * \return APEXCODEC_STATUS_CORRUPTED  if unexpected error has occurred
 */
ApexCodec_Status ApexCodec_Component_process(
        ApexCodec_Component *comp,
        const ApexCodec_Buffer *input,
        ApexCodec_Buffer *output,
        size_t *consumed,
        size_t *produced) __INTRODUCED_IN(36);

/**
 * Configure the component with the given config.
 *
 * Configurations are Codec 2.0 configs in binary blobs,
 * concatenated if there are multiple configs.
 *
 * frameworks/av/media/codec2/core/include/C2Param.h contains more details about the configuration
 * blob layout.
 *
 * The component may correct the configured parameters to the closest supported values, and could
 * fail in case there are no values that the component can auto-correct to. |result| contains the
 * information about the failures. See ApexCodec_SettingResultFailure and ApexCodec_SettingResults
 * for more details.
 *
 * \param [in]    comp   the handle for the component
 * \param [inout] config the config blob; after the call, the config blob is updated to the actual
 *                       config by the component.
 * \param [out]   result the result of the configuration.
 *                       the client should call ApexCodec_SettingResult_getResultAtIndex()
 *                       to extract the result. The result object is owned by the client and should
 *                       be released with ApexCodec_SettingResult_release().
 *                       |result| may be nullptr if empty.
 * \return APEXCODEC_STATUS_OK         if successful
 * \return APEXCODEC_STATUS_BAD_VALUE  if the config is invalid
 * \return APEXCODEC_STATUS_BAD_STATE  if the component is not in the right state to be configured
 * \return APEXCODEC_STATUS_CORRUPTED  if unexpected error has occurred
 */
ApexCodec_Status ApexCodec_Configurable_config(
        ApexCodec_Configurable *comp,
        ApexCodec_LinearBuffer *config,
        ApexCodec_SettingResults **results) __INTRODUCED_IN(36);

/**
 * Query the component for the given indices.
 *
 * Parameter indices are defined in frameworks/av/media/codec2/core/include/C2Config.h.
 *
 * \param [in] comp         the handle for the component
 * \param [in] indices      the array of indices to query
 * \param [in] numIndices   the size of the indices array
 * \param [inout] config    the output buffer for the config blob, allocated by the client.
 *                          if the |config->size| was insufficient, it is set to the required size
 *                          and |config->data| remains unchanged.
 * \param [out] written     the number of bytes written to |config|.
 * \return APEXCODEC_STATUS_OK          if successful
 * \return APEXCODEC_STATUS_NO_MEMORY   if |config.size| is too small; |config.size| is updated to the
 *                                      requested buffer size.
 * \return APEXCODEC_STATUS_BAD_VALUE   if the parameters are bad. e.g. |indices|, |config|,
 *                                      |config->data| or |written| is nullptr.
 */
ApexCodec_Status ApexCodec_Configurable_query(
        ApexCodec_Configurable *comp,
        uint32_t indices[],
        size_t numIndices,
        ApexCodec_LinearBuffer *config,
        size_t *written) __INTRODUCED_IN(36);

/**
 * Struct that represents a parameter descriptor.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_ParamDescriptors ApexCodec_ParamDescriptors;

/**
 * Enum that represents the attributes of a parameter.
 *
 * Introduced in API 36.
 */
typedef enum ApexCodec_ParamAttribute : uint32_t {
    /** parameter is required to be specified */
    APEXCODEC_PARAM_IS_REQUIRED   = 1u << 0,
    /** parameter retains its value */
    APEXCODEC_PARAM_IS_PERSISTENT = 1u << 1,
    /** parameter is strict */
    APEXCODEC_PARAM_IS_STRICT     = 1u << 2,
    /** parameter is read-only */
    APEXCODEC_PARAM_IS_READ_ONLY  = 1u << 3,
    /** parameter shall not be visible to clients */
    APEXCODEC_PARAM_IS_HIDDEN     = 1u << 4,
    /** parameter shall not be used by framework (other than testing) */
    APEXCODEC_PARAM_IS_INTERNAL   = 1u << 5,
    /** parameter is publicly const (hence read-only) */
    APEXCODEC_PARAM_IS_CONST      = 1u << 6 | APEXCODEC_PARAM_IS_READ_ONLY,
} ApexCodec_ParamAttribute;

/**
 * Get the parameter indices of the param descriptors.
 *
 * \param [in] descriptors the param descriptors object
 * \param [out] indices the pointer to be filled with the array of the indices;
 *                      the array is owned by |descriptors| and should not be freed by the client.
 * \param [out] numIndices the size of the indices array
 * \return APEXCODEC_STATUS_OK          if successful
 * \return APEXCODEC_STATUS_BAD_VALUE   if parameters are bad. e.g. |descriptors|, |indices| or
 *                                  |numIndices| is nullptr.
 */
ApexCodec_Status ApexCodec_ParamDescriptors_getIndices(
        ApexCodec_ParamDescriptors *descriptors,
        uint32_t **indices,
        size_t *numIndices) __INTRODUCED_IN(36);

/**
 * Get the descriptor of the param.
 *
 * \param [in] descriptors the param descriptors object
 * \param [in] index the index of the param
 * \param [out] attr the attribute of the param
 * \param [out] name    the pointer to be filled with the name of the param
 *                      the string is owned by |descriptors| and should not be freed by the client.
 * \param [out] dependencies the pointer to be filled with an array of the parameter indices
 *                        that the parameter with |index| depends on.
 *                        may be null if empty.
 *                        the array is owned by |descriptors| and should not be freed by the client.
 * \param [out] numDependencies the number of dependencies
 * \return APEXCODEC_STATUS_OK          if successful
 * \return APEXCODEC_STATUS_BAD_VALUE   if parameters are bad. e.g. |descriptors|, |attr|, |name|,
 *                                  |dependencies| or |numDependencies| is nullptr.
 * \return APEXCODEC_STATUS_BAD_INDEX   if the index is not included in the param descriptors.
 */
ApexCodec_Status ApexCodec_ParamDescriptors_getDescriptor(
        ApexCodec_ParamDescriptors *descriptors,
        uint32_t index,
        ApexCodec_ParamAttribute *attr,
        const char **name,
        uint32_t **dependencies,
        size_t *numDependencies) __INTRODUCED_IN(36);

/**
 * Release the param descriptors object.
 *
 * \param descriptors the param descriptors object
 */
ApexCodec_Status ApexCodec_ParamDescriptors_release(
        ApexCodec_ParamDescriptors *descriptors) __INTRODUCED_IN(36);

/**
 * Query the component for the supported parameters.
 *
 * \param comp the handle for the component
 * \param descriptors   the pointer to be filled with the param descriptors object
 *                      the object should be released with ApexCodec_ParamDescriptors_release().
 * \return APEXCODEC_STATUS_OK          if successful
 * \return APEXCODEC_STATUS_BAD_VALUE   if parameters are bad. e.g. |descriptors| is nullptr.
 */
ApexCodec_Status ApexCodec_Configurable_querySupportedParams(
        ApexCodec_Configurable *comp,
        ApexCodec_ParamDescriptors **descriptors) __INTRODUCED_IN(36);

/**
 * Struct that represents the query for the supported values of a parameter.
 *
 * The offset of the field can be found in the layout of the parameter blob.
 *
 * Introduced in API 36.
 */
typedef struct ApexCodec_SupportedValuesQuery {
    /* in-params */

    /** index of the param */
    uint32_t index;
    /** offset to the param field */
    size_t offset;
    /** query type */
    ApexCodec_SupportedValuesQueryType type;

    /* out-params */

    /** status of the query */
    ApexCodec_Status status;

    /** supported values. must be released with ApexCodec_SupportedValues_release(). */
    ApexCodec_SupportedValues *values;
} ApexCodec_SupportedValuesQuery;

/**
 * Query the component for the supported values of the given indices.
 *
 * \param comp the handle for the component
 * \param queries the array of queries
 * \param numQueries the size of the queries array
 * \return  APEXCODEC_STATUS_OK         if successful
 *          APEXCODEC_STATUS_CORRUPTED  if unexpected error has occurred
 */
ApexCodec_Status ApexCodec_Configurable_querySupportedValues(
        ApexCodec_Configurable *comp,
        ApexCodec_SupportedValuesQuery *queries,
        size_t numQueries) __INTRODUCED_IN(36);

__END_DECLS