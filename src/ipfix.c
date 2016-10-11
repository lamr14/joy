/*
 *
 * Copyright (c) 2016 Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string.h>   /* for memcpy() */
#include "ipfix.h"


#define MAX_IPFIX_TEMPLATES 100
struct ipfix_template ix_templates[MAX_IPFIX_TEMPLATES];
unsigned short num_ipfix_templates = 0;


static inline int ipfix_template_key_cmp(const struct ipfix_template_key a,
                                         const struct ipfix_template_key b) {
  if (a.observe_dom_id == b.observe_dom_id &&
      a.template_id == b.template_id &&
      a.exporter_addr.s_addr == b.exporter_addr.s_addr) {
    return 0;
  } else {
    return 1;
  }
}


/*
 * @brief Construct a flow key corresponding to an IPFIX data record.
 *
 * Create a flow key that can be used to either lookup an existing
 * flow record, or in the process of making a new flow record for
 * storage of the IPFIX data. Note, usage of the function assumes
 * that the \p cur_template contains variable lengths related to
 * fields where necessary.
 *
 * @param key Flow key to be filled in with 5-tuple identifier.
 * @param cur_template IPFIX template that corresponds to data record.
 * @param flow_data IPFIX data record being parsed.
 */
void ipfix_flow_key_init(struct flow_key *key,
                         const struct ipfix_template *cur_template,
                         const void *flow_data) {
  int i;
  for (i = 0; i < cur_template->hdr.field_count; i++) {
    unsigned short field_length = 0;

    if (cur_template->fields[i].variable_length) {
      field_length = cur_template->fields[i].variable_length;
    } else {
      field_length = cur_template->fields[i].fixed_length;
    }

    switch (cur_template->fields[i].info_elem_id) {
      case IPFIX_SOURCE_IPV4_ADDRESS:
        key->sa.s_addr = *(const int *)flow_data;
        flow_data += field_length;
        break;
      case IPFIX_DESTINATION_IPV4_ADDRESS:
        key->da.s_addr = *(const int *)flow_data;
        flow_data += field_length;
        break;
      case IPFIX_SOURCE_TRANSPORT_PORT:
        key->sp = ntohs(*(const short *)flow_data);
        flow_data += field_length;
        break;
      case IPFIX_DESTINATION_TRANSPORT_PORT:
        key->dp = ntohs(*(const short *)flow_data);
        flow_data += field_length;
        break;
      case IPFIX_PROTOCOL_IDENTIFIER:
        key->prot = *(const char *)flow_data;
        flow_data += field_length;
        break;
      default:
        flow_data += field_length;
        break;
    }
  }
}


/*
 * @brief Initialize an IPFIX template key.
 *
 * The template key is used by the IPFIX Collector to uniquely
 * identify templates that it encounters.
 *
 * @param k IPFIX template key structure that will be initialized.
 * @param addr Exporter IP address.
 * @param id Exporter observation domain id.
 * @param template_id Template id contained in the template header.
 */
void ipfix_template_key_init(struct ipfix_template_key *k,
                             unsigned long addr,
                             unsigned long id,
                             unsigned short template_id) {
  *k = (const struct ipfix_template_key){{0}, 0, 0};
  k->exporter_addr.s_addr = addr;
  k->observe_dom_id = id;
  k->template_id = template_id;
}


/*
 * @brief Parse through the contents of an IPFIX Template Set.
 *
 * @param ipfix The IPFIX message header.
 * @param template_start Beginning of the template set.
 * @param set_len Total length of the template set measured in octets. 
 * @param rec_key Flow key generated upstream in process_packet()
 *                corresponding to the packet capture.
 */
int ipfix_parse_template_set(const struct ipfix_hdr *ipfix,
                             const void *template_start,
                             int set_len,
                             const struct flow_key rec_key) {

  const void *template_ptr = template_start;
  int template_set_len = set_len;

  while (template_set_len > 0) {
    const struct ipfix_template_hdr *template_hdr = template_ptr;
    template_ptr += 4; /* Move past template header */
    template_set_len -= 4;
    unsigned short template_id = ntohs(template_hdr->template_id);
    unsigned short field_count = ntohs(template_hdr->field_count);
    struct ipfix_template cur_template;
    struct ipfix_template_key template_key;
    int cur_template_pld_len = 0;
    int redundant_template_pld_len = 0;
    int redundant_template = 0;
    int i;

    /*
     * Define Template Set key:
     * {source IP + observation domain ID + template ID}
     */
    ipfix_template_key_init(&template_key, rec_key.sa.s_addr,
                            ntohl(ipfix->observe_dom_id), template_id);

    /* Check to see if template already exists, if so, continue */
    for (i = 0; i < num_ipfix_templates; i++) {
      if (ipfix_template_key_cmp(template_key,
                                 ix_templates[i].template_key) == 0) {
        redundant_template = 1;
        redundant_template_pld_len = ix_templates[i].payload_length;
        break;
      }
    }

    if (redundant_template) {
      /* Template already exists */
      template_ptr += redundant_template_pld_len;
      template_set_len -= redundant_template_pld_len;
      continue;
    }

    /*
     * The enterprise field may or may not exist for certain fields
     * within the payload, so we need to walk the entire template.
     */
    for (i = 0; i < field_count; i++) {
      int fld_size = 4;
      const struct ipfix_template_field *tmp_field = template_ptr;
      const unsigned short host_info_elem_id = ntohs(tmp_field->info_elem_id);
      const unsigned short host_fixed_length = ntohs(tmp_field->fixed_length);

      if (ipfix_field_enterprise_bit(host_info_elem_id)) {
        /* The enterprise bit is set, remove from element id */
        cur_template.fields[i].info_elem_id = host_info_elem_id ^ 0x8000;
        cur_template.fields[i].enterprise_num = ntohl(tmp_field->enterprise_num);
        fld_size = 8;
      } else {
        cur_template.fields[i].info_elem_id = host_info_elem_id;
      }

      cur_template.fields[i].fixed_length = host_fixed_length;

      template_ptr += fld_size;
      template_set_len -= fld_size;
      cur_template_pld_len += fld_size;
    }

    /* The template is new, so save info */
    cur_template.hdr.template_id = template_id;
    cur_template.hdr.field_count = field_count;
    cur_template.payload_length = cur_template_pld_len;
    cur_template.template_key = template_key;

    /* Save template */
    ix_templates[num_ipfix_templates] = cur_template;
    num_ipfix_templates += 1;
    num_ipfix_templates %= MAX_IPFIX_TEMPLATES;
  }

  return 0;
}


/*
 * @brief Parse through the contents of an IPFIX Data Set.
 *
 * @param ipfix The IPFIX message header.
 * @param template_start Beginning of the data set.
 * @param set_len Total length of the data set measured in octets.
 * @param set_id I.d. of Template to be used for interpreting data set.
 * @param rec_key Flow key generated upstream in process_packet()
 *                corresponding to the packet capture.
 * @param prev_data_key Previous flow key that was created for preceding
 *                      data record. This is a handle to the variable
 *                      sitting on process_ipfix() stack memory.
 */
int ipfix_parse_data_set(const struct ipfix_hdr *ipfix,
                         const void *data_start,
                         int set_len,
                         unsigned short set_id,
                         const struct flow_key rec_key,
                         struct flow_key *prev_data_key) {

  const unsigned char *data_ptr = data_start;
  int data_set_len = set_len;
  unsigned short template_id = set_id;
  struct ipfix_template_key template_key;
  struct ipfix_template *cur_template = NULL;
  int i;

  /* Define data template key:
   * {source IP + observation domain ID + template ID}
   */
  ipfix_template_key_init(&template_key, rec_key.sa.s_addr,
                          htonl(ipfix->observe_dom_id), template_id);

  /* Look for template match */
  for (i = 0; i < num_ipfix_templates; i++) {
    if (ipfix_template_key_cmp(template_key,
                               ix_templates[i].template_key) == 0) {
      cur_template = &ix_templates[i];
      break;
    }
  }

  /* Process data if we know the template */
  if (cur_template != NULL) {
    unsigned short data_field_count = cur_template->hdr.field_count;
    int data_record_size = 0;
    int data_records_in_set = 0;
    const unsigned char *flow_data = NULL;
    struct flow_key key;
    struct flow_record *ix_record;

    /*
     * Calculate the size of the data records
     * corresponding to this particular template.
     */
    flow_data = data_ptr; /* Point to beginning of this record */
    for (i = 0; i < data_field_count; i++) {
      unsigned short actual_fld_len = 0;
      unsigned short cur_fld_len = cur_template->fields[i].fixed_length;
      if (cur_fld_len == 65535) {
        /* The current field is of variable length */
        unsigned char fld_len_flag = (unsigned char)*data_ptr;
        if (fld_len_flag < 255) {
          actual_fld_len = (unsigned short)fld_len_flag;
          /* Fill in the variable length field in global template list */
          cur_template->fields[i].variable_length = actual_fld_len;
        } else if (fld_len_flag == 255) {
          actual_fld_len = ntohs(*(unsigned short *)(data_ptr + 1));
          /* Fill in the variable length field in global template list */
          cur_template->fields[i].variable_length = actual_fld_len;
        } else {
          /* Error, invalid variable length */
          printf("Error: bad variable length\n");
          return 1;
        }
      } else {
        /* Fixed length field */
        actual_fld_len = cur_fld_len;
      }
      data_ptr += actual_fld_len;
      data_record_size += actual_fld_len;
    }

    /* Reset the data pointer to beginning of first record */
    data_ptr = flow_data;

    /* Process all data records in set */
    for (data_records_in_set = 0;
         data_records_in_set < (data_set_len/data_record_size);
         data_records_in_set++){
      flow_data = data_ptr;

      /* Init flow key */
      ipfix_flow_key_init(&key, cur_template, flow_data);

      /* Get a flow record related to ipfix data */
      ix_record = flow_key_get_record(&key, CREATE_RECORDS);

      /* Fill out record */
      if (memcmp(&key, prev_data_key, sizeof(struct flow_key)) != 0) {
        //ipfix_process_flow_record(ix_record, cur_template, flow_data, 0);
      } else {
        //ipfix_process_flow_record(ix_record, cur_template, flow_data, 1);
      }
      memcpy(prev_data_key, &key, sizeof(struct flow_key));

      data_ptr += data_record_size;
      printf("End of ipfix_parse_data_set :)\n");
    }
  } else {
    /* FIXME hold onto the data set for a certain amount of time since
     * the template may come later... */
    printf("Error: current template is null, cannot parse the data set\n");
  }

  return 0;
}


void ipfix_process_flow_record(struct flow_record *ix_record,
                               const struct ipfix_template *cur_template,
                               const void *flow_data,
                               int record_num) {
  /* TODO Implement this function */
  ;
}
