/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "cycling_power_service_server.c"


#include "bluetooth.h"
#include "btstack_defines.h"
#include "ble/att_db.h"
#include "ble/att_server.h"
#include "btstack_util.h"
#include "bluetooth_gatt.h"
#include "btstack_debug.h"
#include "l2cap.h"

#include "ble/gatt-service/cycling_power_service_server.h"

// error codes from cps spec
#define CYCLING_POWER_ERROR_CODE_INAPPROPRIATE_CONNECTION_PARAMETERS	0x80

typedef enum {
	CP_OPCODE_IDLE = 0,
	CP_OPCODE_SET_CUMULATIVE_VALUE,
	CP_OPCODE_UPDATE_SENSOR_LOCATION,
	CP_OPCODE_REQUEST_SUPPORTED_SENSOR_LOCATIONS,
	CP_OPCODE_SET_CRANK_LENGTH,
	CP_OPCODE_REQUEST_CRANK_LENGTH,
	CP_OPCODE_SET_CHAIN_LENGTH,
	CP_OPCODE_REQUEST_CHAIN_LENGTH,
	CP_OPCODE_SET_CHAIN_WEIGHT,
	CP_OPCODE_REQUEST_CHAIN_WEIGHT,
	CP_OPCODE_SET_SPAN_LENGTH,
	CP_OPCODE_REQUEST_SPAN_LENGTH,
	CP_OPCODE_START_OFFSET_COMPENSATION,
	CP_OPCODE_MASK_CYCLING_POWER_MEASUREMENT_CHARACTERISTIC_CONTENT,
	CP_OPCODE_REQUEST_SAMPLING_RATE,
	CP_OPCODE_REQUEST_FACTORY_CALIBRATION_DATE,
	CP_OPCODE_START_ENHANCED_OFFSET_COMPENSATION,
	CP_OPCODE_RESPONSE_CODE = 32
} cycling_power_opcode_t;

typedef enum {
	CP_RESPONSE_VALUE_SUCCESS = 1,
	CP_RESPONSE_VALUE_OP_CODE_NOT_SUPPORTED,
	CP_RESPONSE_VALUE_INVALID_PARAMETER,
	CP_RESPONSE_VALUE_OPERATION_FAILED
} cycling_power_response_value_t;

typedef struct {
	hci_con_handle_t con_handle;

	// Cycling Power Measurement 
	uint16_t measurement_value_handle;
	uint16_t measurement_flags;					// see cycling_pover_measurement_flag_t
	int16_t  instantaneous_power_watt;
	uint8_t  pedal_power_balance_percentage; 	// percentage, resolution 1/2
	uint16_t accumulated_torque_m; 				// meters, resolution 1/32
	// wheel revolution data:
	uint32_t cumulative_wheel_revolutions;
	uint16_t last_wheel_event_time_s; 			// seconds, resolution 1/2048
	// crank revolution data:
	uint32_t cumulative_crank_revolutions;
	uint16_t last_crank_event_time_s; 			// seconds, resolution 1/1024
	// extreme force magnitudes
	int16_t maximum_force_magnitude_newton;
	int16_t minimum_force_magnitude_newton;
	int16_t maximum_torque_magnitude_newton_m; 	// newton meters, resolution 1/32
	int16_t minimum_torque_magnitude_newton_m; 	// newton meters, resolution 1/32
	// extreme angles
	uint16_t maximum_angle_deg;					// 12bit, degrees
	uint16_t minimum_angle_deg;					// 12bit, degrees, concatenated with previous into 3 octets
												// i.e. if the Maximum Angle is 0xABC and the Minimum Angle is 0x123, the transmitted value is 0x123ABC.
	uint16_t top_dead_spot_angle;
	uint16_t bottom_dead_spot_angle;
	uint16_t accumulated_energy_kJ;				// kilojoules

	// CP Measurement Notification (Client Characteristic Configuration)
	uint16_t measurement_client_configuration_descriptor_handle;
	uint16_t measurement_client_configuration_descriptor_notify;
	btstack_context_callback_registration_t measurement_notify_callback;

	// CP Measurement Broadcast (Server Characteristic Configuration)
	uint16_t measurement_server_configuration_descriptor_handle;
	uint16_t measurement_server_configuration_descriptor_broadcast;
	btstack_context_callback_registration_t measurement_broadcast_callback;

	// Cycling Power Feature
	uint16_t feature_value_handle;
	uint32_t feature_flags; 							// see cycling_power_feature_flag_t
	
	// Sensor Location
	uint16_t sensor_location_value_handle;
	cycling_power_sensor_location_t sensor_location; 	// see cycling_power_sensor_location_t
	
	// Cycling Power Vector
	uint16_t vector_value_handle;
	uint8_t  vector_flags;												// see cycling_power_vector_flag_t
	uint16_t vector_cumulative_crank_revolutions;
	uint16_t vector_last_crank_event_time_s;							// seconds, resolution 1/1024
	uint16_t vector_first_crank_measurement_angle_deg;
	int16_t  vector_instantaneous_force_magnitude_newton_array;			// newton
	int16_t  vector_instantaneous_torque_magnitude_newton_per_m_array;	// newton per meter, resolution 1/32

	// CP Vector Notification (Client Characteristic Configuration)
	uint16_t vector_client_configuration_descriptor_handle;
	uint16_t vector_client_configuration_descriptor_notify;
	btstack_context_callback_registration_t vector_notify_callback;

	// CP Control Point
	uint16_t control_point_value_handle;
	// CP Control Point Indication (Client Characteristic Configuration)
	uint16_t control_point_client_configuration_descriptor_handle;
	uint16_t control_point_client_configuration_descriptor_indicate;
	btstack_context_callback_registration_t control_point_indicate_callback;

	cycling_power_opcode_t request_opcode;
	cycling_power_response_value_t response_value;
} cycling_power_t;

static att_service_handler_t cycling_power_service;
static cycling_power_t cycling_power;

static uint16_t cycling_power_service_read_callback(hci_con_handle_t con_handle, uint16_t attribute_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size){
	UNUSED(con_handle);
	UNUSED(attribute_handle);
	UNUSED(offset);
	cycling_power_t * instance = &cycling_power;

	if (attribute_handle == instance->measurement_client_configuration_descriptor_handle){
		if (buffer && buffer_size >= 2){
			little_endian_store_16(buffer, 0, instance->measurement_client_configuration_descriptor_notify);
		} 
		return 2;
	}

	if (attribute_handle == instance->measurement_server_configuration_descriptor_handle){
		if (buffer && buffer_size >= 2){
			little_endian_store_16(buffer, 0, instance->measurement_server_configuration_descriptor_broadcast);
		} 
		return 2;
	}

	if (attribute_handle == instance->vector_client_configuration_descriptor_handle){
		if (buffer && buffer_size >= 2){
			little_endian_store_16(buffer, 0, instance->vector_client_configuration_descriptor_notify);
		} 
		return 2;
	}

	if (attribute_handle == instance->control_point_client_configuration_descriptor_handle){
		if (buffer && buffer_size >= 2){
			little_endian_store_16(buffer, 0, instance->control_point_client_configuration_descriptor_indicate);
		} 
		return 2;
	}

	if (attribute_handle == instance->feature_value_handle){
		return 2;
	}	
	
	if (attribute_handle == instance->sensor_location_value_handle){
		if (buffer && buffer_size >= 1){
			buffer[0] = instance->sensor_location;
		} 
		return 1;
	}	
	return 0;
}

static void cycling_power_service_measurement_can_send_now(void * context){
	cycling_power_t * instance = (cycling_power_t *) context;
	if (!instance){
		printf("instance is null (cycling_power_service_measurement_can_send_now)\n");
		return;
	}
	uint8_t value[11];
	int pos = 0;

	att_server_notify(instance->con_handle, instance->measurement_value_handle, &value[0], pos); 
}

static void cycling_power_service_response_can_send_now(void * context){
	cycling_power_t * instance = (cycling_power_t *) context;
	if (!instance){
		printf("instance is null (cycling_power_service_measurement_can_send_now)\n");
		return;
	}
		
	uint8_t value[3 + sizeof(cycling_power_sensor_location_t)];
	int pos = 0;
	value[pos++] = CP_OPCODE_RESPONSE_CODE;
	value[pos++] = instance->request_opcode;
	value[pos++] = instance->response_value;
	switch (instance->request_opcode){
		default:
			break;
	}
	cycling_power_opcode_t temp_request_opcode = instance->request_opcode;
	instance->request_opcode = CP_OPCODE_IDLE;

	uint8_t status = att_server_indicate(instance->con_handle, instance->control_point_value_handle, &value[0], pos); 
	printf("att_server_indicate status 0x%02x\n", status);
	
	switch (temp_request_opcode){
		// todo handle notify if needed
 		default:
			break;
	}
}

static int cycling_power_service_write_callback(hci_con_handle_t con_handle, uint16_t attribute_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size){
	UNUSED(con_handle);
	UNUSED(transaction_mode);
	UNUSED(offset);
	UNUSED(buffer_size);
	cycling_power_t * instance = &cycling_power;

	// printf("cycling_power_service_write_callback: attr handle 0x%02x\n", attribute_handle);
	if (attribute_handle == instance->measurement_client_configuration_descriptor_handle){
		if (buffer_size < 2){
			return ATT_ERROR_INVALID_OFFSET;
		}
		instance->measurement_client_configuration_descriptor_notify = little_endian_read_16(buffer, 0);
		instance->con_handle = con_handle;
		if (instance->measurement_client_configuration_descriptor_notify){
			printf("measurement enable notification\n");
		}
		return 0;
	}

	if (attribute_handle == instance->measurement_server_configuration_descriptor_handle){
		if (buffer_size < 2){
			return ATT_ERROR_INVALID_OFFSET;
		}
		instance->measurement_server_configuration_descriptor_broadcast = little_endian_read_16(buffer, 0);
		instance->con_handle = con_handle;
		if (instance->measurement_server_configuration_descriptor_broadcast){
			printf("measurement enable broadcast\n");
		}
		return 0;
	}

	if (attribute_handle == instance->vector_client_configuration_descriptor_handle){
		if (buffer_size < 2){
			return ATT_ERROR_INVALID_OFFSET;
		}
		instance->vector_client_configuration_descriptor_notify = little_endian_read_16(buffer, 0);
		instance->con_handle = con_handle;
		if (instance->vector_client_configuration_descriptor_notify){
			printf("vector enable notification\n");
		}
		return 0;
	}

	if (attribute_handle == instance->control_point_client_configuration_descriptor_handle){
		if (buffer_size < 2){
			return ATT_ERROR_INVALID_OFFSET;
		}
		instance->control_point_client_configuration_descriptor_indicate = little_endian_read_16(buffer, 0);
		instance->con_handle = con_handle;
		if (instance->control_point_client_configuration_descriptor_indicate){
			printf("control point enable indication\n");
		}
		return 0;
	}

	if (attribute_handle == instance->control_point_value_handle){
		// if (instance->control_point_client_configuration_descriptor_indicate == 0) return CSC_ERROR_CODE_CCC_DESCRIPTOR_IMPROPERLY_CONFIGURED;
		// if (instance->request_opcode != CSC_OPCODE_IDLE) return CSC_ERROR_CODE_PROCEDURE_ALREADY_IN_PROGRESS;

		instance->request_opcode = buffer[0];
		instance->response_value = CP_RESPONSE_VALUE_SUCCESS;
		
		switch (instance->request_opcode){
			default:
				break;
		}
		printf("control point, opcode %02x, response %02x\n", instance->request_opcode, instance->response_value);
	
		if (instance->control_point_client_configuration_descriptor_indicate){
			instance->control_point_indicate_callback.callback = &cycling_power_service_response_can_send_now;
			instance->control_point_indicate_callback.context  = (void*) instance;
			att_server_register_can_send_now_callback(&instance->control_point_indicate_callback, instance->con_handle);
		}
		return 0;
	}

	printf("write callback, not handeled read on handle 0x%02x\n", attribute_handle);
	return 0;
}

void cycling_power_service_server_init(void){
	cycling_power_t * instance = &cycling_power;
	
	instance->sensor_location = CP_SENSOR_LOCATION_OTHER;

	// get service handle range
	uint16_t start_handle = 0;
	uint16_t end_handle   = 0xffff;
	int service_found = gatt_server_get_get_handle_range_for_service_with_uuid16(ORG_BLUETOOTH_SERVICE_CYCLING_POWER, &start_handle, &end_handle);
	if (!service_found){
		printf("no service found\n");
		return;
	}
	// get CP Mesurement characteristic value handle and client configuration handle
	instance->measurement_value_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_MEASUREMENT);
	instance->measurement_client_configuration_descriptor_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_MEASUREMENT);
	instance->measurement_server_configuration_descriptor_handle = gatt_server_get_server_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_MEASUREMENT);
	
	// get CP Feature characteristic value handle and client configuration handle
	instance->feature_value_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_FEATURE);
	// get CP Sensor Location characteristic value handle and client configuration handle
	instance->sensor_location_value_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SENSOR_LOCATION);
	
	// get CP Vector characteristic value handle and client configuration handle
	instance->vector_value_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_VECTOR);
	instance->vector_client_configuration_descriptor_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_VECTOR);

	// get Body Sensor Location characteristic value handle and client configuration handle
	instance->sensor_location_value_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SENSOR_LOCATION);
	
	// get SP Control Point characteristic value handle and client configuration handle
	instance->control_point_value_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_CONTROL_POINT);
	instance->control_point_client_configuration_descriptor_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_CYCLING_POWER_CONTROL_POINT);

	printf("Measurement     value handle 0x%02x\n", instance->measurement_value_handle);
	printf("M. Client Cfg   value handle 0x%02x\n", instance->measurement_client_configuration_descriptor_handle);
	printf("M. Server Cfg   value handle 0x%02x\n", instance->measurement_server_configuration_descriptor_handle);

	printf("Feature         value handle 0x%02x\n", instance->feature_value_handle);
	printf("Sensor location value handle 0x%02x\n", instance->sensor_location_value_handle);

	printf("Vector          value handle 0x%02x\n", instance->vector_value_handle);
	printf("Vector Cfg.     value handle 0x%02x\n", instance->vector_client_configuration_descriptor_handle);

	printf("Control Point   value handle 0x%02x\n", instance->control_point_value_handle);
	printf("Control P. Cfg. value handle 0x%02x\n", instance->control_point_client_configuration_descriptor_handle);
	
	cycling_power_service.start_handle   = start_handle;
	cycling_power_service.end_handle     = end_handle;
	cycling_power_service.read_callback  = &cycling_power_service_read_callback;
	cycling_power_service.write_callback = &cycling_power_service_write_callback;
	
	att_server_register_service_handler(&cycling_power_service);
}

void cycling_power_service_server_update_values(void){
	cycling_power_t * instance = &cycling_power;
	
	// TODO: update values
	if (instance->measurement_client_configuration_descriptor_notify){
		instance->measurement_notify_callback.callback = &cycling_power_service_measurement_can_send_now;
		instance->measurement_notify_callback.context  = (void*) instance;
		// printf("cycling_power_service_server_update_values instance %p, context %p\n", instance, instance->measurement_notify_callback.context);
		att_server_register_can_send_now_callback(&instance->measurement_notify_callback, instance->con_handle);
	}
}