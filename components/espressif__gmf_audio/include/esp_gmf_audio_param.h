/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include "esp_gmf_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Set frame destination sample rate for audio element
 *
 * @param[in]  handle     Audio element handle
 * @param[in]  dest_rate  Destination sample rate
 *
 * @return
 *       - ESP_GMF_ERR_OK           On success
 *       - ESP_GMF_ERR_NOT_FOUND    Not found the method
 *       - ESP_GMF_ERR_MEMORY_LACK  Failed to allocate memory
 *       - Others                   Failed to apply method
 */
esp_gmf_err_t esp_gmf_audio_param_set_dest_rate(esp_gmf_element_handle_t self, uint32_t dest_rate);

/**
 * @brief  Set frame destination bits for audio element
 *
 * @param[in]  handle     Audio element handle
 * @param[in]  dest_bits  Destination bits
 *
 * @return
 *       - ESP_GMF_ERR_OK           On success
 *       - ESP_GMF_ERR_NOT_FOUND    Not found the method
 *       - ESP_GMF_ERR_MEMORY_LACK  Failed to allocate memory
 *       - Others                   Failed to apply method
 */
esp_gmf_err_t esp_gmf_audio_param_set_dest_bits(esp_gmf_element_handle_t self, uint8_t dest_bits);

/**
 * @brief  Set frame destination channel for audio element
 *
 * @param[in]  handle   Audio element handle
 * @param[in]  dest_ch  Destination channel
 *
 * @return
 *       - ESP_GMF_ERR_OK           On success
 *       - ESP_GMF_ERR_NOT_FOUND    Not found the method
 *       - ESP_GMF_ERR_MEMORY_LACK  Failed to allocate memory
 *       - Others                   Failed to apply method
 */
esp_gmf_err_t esp_gmf_audio_param_set_dest_ch(esp_gmf_element_handle_t self, uint8_t dest_ch);

#ifdef __cplusplus
}
#endif
