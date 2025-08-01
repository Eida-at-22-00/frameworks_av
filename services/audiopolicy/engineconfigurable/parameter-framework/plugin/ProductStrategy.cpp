/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "ProductStrategy.h"
#include "PolicyMappingKeys.h"
#include "PolicySubsystem.h"

ProductStrategy::ProductStrategy(const std::string &mappingValue,
                   CInstanceConfigurableElement *instanceConfigurableElement,
                   const CMappingContext &context,
                   core::log::Logger& logger)
    : CFormattedSubsystemObject(instanceConfigurableElement,
                                logger,
                                mappingValue,
                                MappingKeyAmend1,
                                (MappingKeyAmendEnd - MappingKeyAmend1 + 1),
                                context) {

    ALOG_ASSERT(instanceConfigurableElement != nullptr, "Invalid Configurable Element");
    mPolicySubsystem = static_cast<const PolicySubsystem *>(
            instanceConfigurableElement->getBelongingSubsystem());
    ALOG_ASSERT(mPolicySubsystem != nullptr, "Invalid Policy Subsystem");

    mPolicyPluginInterface = mPolicySubsystem->getPolicyPluginInterface();
    ALOG_ASSERT(mPolicyPluginInterface != nullptr, "Invalid Policy Plugin Interface");

    const std::string nameFromStructure(context.getItem(MappingKeyName));
    std::string name;
    if (context.iSet(MappingKeyIdentifier)) {
        size_t id = context.getItemAsInteger(MappingKeyIdentifier);
        mId = static_cast<android::product_strategy_t>(id);
        name = mPolicyPluginInterface->getProductStrategyName(mId);
        if (name.empty()) {
            name = nameFromStructure;
            mId = mPolicyPluginInterface->getProductStrategyByName(name);
        }
    } else {
        name = nameFromStructure;
        mId = mPolicyPluginInterface->getProductStrategyByName(nameFromStructure);
    }

    ALOG_ASSERT(mId != PRODUCT_STRATEGY_NONE, "Product Strategy %s not found", name.c_str());

    ALOGE("Product Strategy %s added", name.c_str());
}

bool ProductStrategy::sendToHW(std::string & /*error*/)
{
    Device deviceParams;
    blackboardRead(&deviceParams, sizeof(deviceParams));

    mPolicyPluginInterface->setDeviceTypesForProductStrategy(mId, deviceParams.applicableDevice);
    mPolicyPluginInterface->setDeviceAddressForProductStrategy(mId, deviceParams.deviceAddress);
    return true;
}
