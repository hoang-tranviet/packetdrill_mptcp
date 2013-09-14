/**
 * Authors: Arnaud Schils
 */

#include "mptcp.h"

void init_mp_state()
{
	mp_state.packetdrill_key_set = false;
	mp_state.kernel_key_set = false;
	queue_init(&mp_state.vars_queue);
	mp_state.vars = NULL; //Init hashmap
	mp_state.last_packetdrill_addr_id = 0;
}

void free_mp_state(){
	free_var_queue();
	free_vars();
	free_flows();
}

/**
 * Remember mptcp connection key generated by packetdrill. This key is needed
 * during the entire mptcp connection and is common among all mptcp subflows.
 */
void set_packetdrill_key(u64 sender_key)
{
	mp_state.packetdrill_key = sender_key;
	mp_state.packetdrill_key_set = true;
}

/**
 * Remember mptcp connection key generated by kernel. This key is needed
 * during the entire mptcp connection and is common among all mptcp subflows.
 */
void set_kernel_key(u64 receiver_key)
{
    mp_state.kernel_key = receiver_key;
    mp_state.kernel_key_set = true;
}

/* var_queue functions */

/**
 * Insert a COPY of name char* in mp_state.vars_queue.
 * Error is returned if queue is full.
 *
 */
int enqueue_var(char *name)
{
	unsigned name_length = strlen(name);
	char *new_el = malloc(sizeof(char)*name_length);
	memcpy(new_el, name, name_length);
	int full_err = queue_enqueue(&mp_state.vars_queue, new_el);
	return full_err;
}

//Caller should free
int dequeue_var(char **name){
	int empty_err = queue_dequeue(&mp_state.vars_queue, (void**)name);
	return empty_err;
}

//Free all variables names (char*) in vars_queue
void free_var_queue()
{
	queue_free(&mp_state.vars_queue);
}

/* hashmap functions */

void save_mp_var_name(char *name, struct mp_var *var)
{
	unsigned name_length = strlen(name);
	var->name = malloc(sizeof(char)*(name_length+1));
	memcpy(var->name, name, (name_length+1)); //+1 to copy '/0' too
}

/**
 *
 * Save a variable <name, value> in variables hashmap.
 * Where value is of u64 type key.
 *
 * Key memory location should stay valid, name is copied.
 *
 */
void add_mp_var_key(char *name, u64 *key)
{
	struct mp_var *var = malloc(sizeof(struct mp_var));
	save_mp_var_name(name, var);
	var->value = key;
	var->mptcp_subtype = MP_CAPABLE_SUBTYPE;
	var->mp_capable_info.script_defined = false;
	add_mp_var(var);
}

/**
 * Save a variable <name, value> in variables hashmap.
 * Value is copied in a newly allocated pointer and will be freed when
 * free_vars function will be executed.
 *
 */
void add_mp_var_script_defined(char *name, void *value, u32 length)
{
	struct mp_var *var = malloc(sizeof(struct mp_var));
	save_mp_var_name(name, var);
	var->value = malloc(length);
	memcpy(var->value, value, length);
	var->mptcp_subtype = MP_CAPABLE_SUBTYPE;
	var->mp_capable_info.script_defined = true;
	add_mp_var(var);
}

/**
 * Add var to the variable hashmap.
 */
void add_mp_var(struct mp_var *var)
{
	HASH_ADD_KEYPTR(hh, mp_state.vars, var->name, strlen(var->name), var);
}

/**
 * Search in the hashmap for the value of the variable of name "name" and
 * return both variable - value (mp_var struct).
 * NULL is returned if not found
 */
struct mp_var *find_mp_var(char *name)
{
    struct mp_var *found;
    HASH_FIND_STR(mp_state.vars, name, found);
    return found;
}

/**
 * Gives next mptcp key value needed to insert variable values while processing
 * the packets.
 */
u64 *find_next_key(){
	char *var_name;
	if(dequeue_var(&var_name) || !var_name){
		return NULL;
	}

	struct mp_var *var = find_mp_var(var_name);
	free(var_name);

	if(!var || var->mptcp_subtype != MP_CAPABLE_SUBTYPE){
		return NULL;
	}
	return (u64*)var->value;
}

/**
 * Iterate through hashmap, free mp_var structs and mp_var->name.
 * Value is not freed for KEY type, since values come from stack.
 */
void free_vars()
{
	struct mp_var *next, *var;
	var = mp_state.vars;

	while(var){
		next = var->hh.next;
		free(var->name);
		if(var->mptcp_subtype == MP_CAPABLE_SUBTYPE){
			if(var->mp_capable_info.script_defined)
				free(var->value);
		}
		free(var);
		var = next;
	}
}

/**
 * @pre inbound packet should be the first packet of a three-way handshake
 * mp_join initiated by packetdrill (thus an inbound mp_join syn packet).
 *
 * @post
 * - Create a new subflow structure containing all available information at this
 * time (src_ip, dst_ip, src_port, dst_port, packetdrill_rand_nbr,
 * packetdrill_addr_id). kernel_addr_id and kernel_rand_nbr should be set when
 * receiving syn+ack with mp_join mptcp option from kernel.
 *
 * - last_packetdrill_addr_id is incremented.
 */
struct mp_subflow *new_subflow_inbound(struct packet *inbound_packet)
{

	struct mp_subflow *subflow = malloc(sizeof(struct mp_subflow));

	if(inbound_packet->ipv4){
		ip_from_ipv4(&inbound_packet->ipv4->src_ip, &subflow->src_ip);
		ip_from_ipv4(&inbound_packet->ipv4->dst_ip, &subflow->dst_ip);
	}

	else if(inbound_packet->ipv6){
		ip_from_ipv6(&inbound_packet->ipv6->src_ip, &subflow->src_ip);
		ip_from_ipv6(&inbound_packet->ipv6->dst_ip, &subflow->dst_ip );
	}

	else{
		return NULL;
	}

	subflow->src_port =	ntohs(inbound_packet->tcp->src_port);
	subflow->dst_port = ntohs(inbound_packet->tcp->dst_port);
	subflow->packetdrill_rand_nbr =	generate_32();
	subflow->packetdrill_addr_id = mp_state.last_packetdrill_addr_id;
	mp_state.last_packetdrill_addr_id++;
	subflow->subflow_sequence_number = 0;
	subflow->next = mp_state.subflows;
	mp_state.subflows = subflow;

	return subflow;
}

struct mp_subflow *new_subflow_outbound(struct packet *outbound_packet)
{

	struct mp_subflow *subflow = malloc(sizeof(struct mp_subflow));
	struct tcp_option *mp_join_syn =
			get_tcp_option(outbound_packet, TCPOPT_MPTCP);

	if(!mp_join_syn)
		return NULL;

	if(outbound_packet->ipv4){
		ip_from_ipv4(&outbound_packet->ipv4->dst_ip, &subflow->src_ip);
		ip_from_ipv4(&outbound_packet->ipv4->src_ip, &subflow->dst_ip);
	}

	else if(outbound_packet->ipv6){
		ip_from_ipv6(&outbound_packet->ipv6->dst_ip, &subflow->src_ip);
		ip_from_ipv6(&outbound_packet->ipv6->src_ip, &subflow->dst_ip);
	}

	else{
		return NULL;
	}

	subflow->src_port =	ntohs(outbound_packet->tcp->dst_port);
	subflow->dst_port = ntohs(outbound_packet->tcp->src_port);
	subflow->kernel_rand_nbr =
			mp_join_syn->data.mp_join.syn.no_ack.sender_random_number;
	subflow->kernel_addr_id =
			mp_join_syn->data.mp_join.syn.address_id;
	subflow->subflow_sequence_number = 0;
	subflow->next = mp_state.subflows;
	mp_state.subflows = subflow;

	return subflow;
}

/**
 * Return the first subflow S of mp_state.subflows for which match(packet, S)
 * returns true.
 */
struct mp_subflow *find_matching_subflow(struct packet *packet,
		bool (*match)(struct mp_subflow*, struct packet*))
{
	struct mp_subflow *subflow = mp_state.subflows;
	while(subflow){
		if((*match)(subflow, packet)){
			return subflow;
		}
		subflow = subflow->next;
	}
	return NULL;
}

static bool does_subflow_match_outbound_packet(struct mp_subflow *subflow,
		struct packet *outbound_packet){
	return subflow->dst_port == ntohs(outbound_packet->tcp->src_port) &&
			subflow->src_port == ntohs(outbound_packet->tcp->dst_port);
}

struct mp_subflow *find_subflow_matching_outbound_packet(
		struct packet *outbound_packet)
{
	return find_matching_subflow(outbound_packet, does_subflow_match_outbound_packet);
}

static bool does_subflow_match_inbound_packet(struct mp_subflow *subflow,
		struct packet *inbound_packet){
	return subflow->dst_port == ntohs(inbound_packet->tcp->dst_port) &&
			subflow->src_port == ntohs(inbound_packet->tcp->src_port);
}

struct mp_subflow *find_subflow_matching_inbound_packet(
		struct packet *inbound_packet)
{
	return find_matching_subflow(inbound_packet, does_subflow_match_inbound_packet);
}

struct mp_subflow *find_subflow_matching_socket(struct socket *socket){
	struct mp_subflow *subflow = mp_state.subflows;
	while(subflow){
		if(subflow->dst_port == socket->live.remote.port &&
				subflow->src_port == socket->live.local.port){
			return subflow;
		}
		subflow = subflow->next;
	}
	return NULL;
}

/**
 * Free all mptcp subflows struct being a member of mp_state.subflows list.
 */
void free_flows(){
	struct mp_subflow *subflow = mp_state.subflows;
	struct mp_subflow *temp;
	while(subflow){
		temp = subflow->next;
		free(subflow);
		subflow = temp;
	}
}

/**
 * Generate a mptcp packetdrill side key and save it for later reference in
 * the script.
 *
 */
int mptcp_gen_key()
{

	//Retrieve variable name parsed by bison.
	char *snd_var_name;
	if(queue_front(&mp_state.vars_queue, (void**)&snd_var_name))
		return STATUS_ERR;

	//Is that var has already a value assigned in the script by user, or should
	//we generate a mptcp key ourselves?
	struct mp_var *snd_var = find_mp_var(snd_var_name);

	if(snd_var && snd_var->mptcp_subtype == MP_CAPABLE_SUBTYPE &&
			snd_var->mp_capable_info.script_defined)
		set_packetdrill_key(*(u64*)snd_var->value);

	//First inbound mp_capable, generate new key
	//and save corresponding variable
	if(!mp_state.packetdrill_key_set){
		seed_generator();
		u64 key = rand_64();
		set_packetdrill_key(key);
		add_mp_var_key(snd_var_name, &mp_state.packetdrill_key);
	}

	return STATUS_OK;
}

/**
 * Insert key field value of mp_capable_syn mptcp option according to variable
 * specified in user script.
 *
 */
int mptcp_set_mp_cap_syn_key(struct tcp_option *tcp_opt)
{
	u64 *key = find_next_key();
	if(!key)
		return STATUS_ERR;
	tcp_opt->data.mp_capable.syn.key = *key;
	return STATUS_OK;
}

/**
 * Insert keys fields values of mp_capable mptcp option according to variables
 * specified in user script.
 */
int mptcp_set_mp_cap_keys(struct tcp_option *tcp_opt)
{
	u64 *key = find_next_key();
	if(!key)
		return STATUS_ERR;
	tcp_opt->data.mp_capable.no_syn.sender_key = *key;

	key = find_next_key();
	if(!key)
		return STATUS_ERR;
	tcp_opt->data.mp_capable.no_syn.receiver_key = *key;
	return STATUS_OK;
}

/**
 * Extract mptcp connection informations from mptcp packets sent by kernel.
 * (For example kernel mptcp key).
 */
static int extract_and_set_kernel_key(
		struct packet *live_packet)
{

	struct tcp_option* mpcap_opt =
			get_tcp_option(live_packet, TCPOPT_MPTCP);

	if(!mpcap_opt)
		return STATUS_ERR;

	//Check if kernel key hasn't been specified by user in script
	char *var_name;
	if(!queue_front(&mp_state.vars_queue, (void**)&var_name)){
		struct mp_var *var = find_mp_var(var_name);
		if(var && var->mptcp_subtype == MP_CAPABLE_SUBTYPE &&
				var->mp_capable_info.script_defined)
			set_kernel_key(*(u64*)var->value);
	}

	if(!mp_state.kernel_key_set){

		//Set found kernel key
		set_kernel_key(mpcap_opt->data.mp_capable.syn.key);
		//Set front queue variable name to refer to kernel key
		char *var_name;
		if(queue_front(&mp_state.vars_queue, (void**)&var_name)){
			return STATUS_ERR;
		}
		add_mp_var_key(var_name, &mp_state.kernel_key);
	}

	return STATUS_OK;
}

/**
 * Insert appropriate key in mp_capable mptcp option.
 */
int mptcp_subtype_mp_capable(struct packet *packet_to_modify,
		struct packet *live_packet,
		struct tcp_option *tcp_opt_to_modify,
		unsigned direction)
{
	int error;
	if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE_SYN &&
			packet_to_modify->tcp->syn &&
			direction == DIRECTION_INBOUND &&
			!packet_to_modify->tcp->ack){
		error = mptcp_gen_key();
		error = mptcp_set_mp_cap_syn_key(tcp_opt_to_modify) || error;
	}

	else if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE_SYN &&
			packet_to_modify->tcp->syn &&
			direction == DIRECTION_OUTBOUND){
		error = extract_and_set_kernel_key(live_packet);
		error = mptcp_set_mp_cap_syn_key(tcp_opt_to_modify);
	}

	else if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE &&
			!packet_to_modify->tcp->syn &&
			packet_to_modify->tcp->ack){
		error = mptcp_set_mp_cap_keys(tcp_opt_to_modify);
		mp_state.initial_dsn = sha1_least_64bits(mp_state.packetdrill_key);
		if(direction == DIRECTION_INBOUND)
			new_subflow_inbound(packet_to_modify);
		else if(direction == DIRECTION_OUTBOUND)
			new_subflow_outbound(packet_to_modify);
		else
			return STATUS_ERR;
	}

	else if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE_SYN &&
			packet_to_modify->tcp->syn &&
			direction == DIRECTION_INBOUND &&
			packet_to_modify->tcp->ack){
		error = mptcp_gen_key();
		error = mptcp_set_mp_cap_syn_key(tcp_opt_to_modify) || error;
	}

	else{
		return STATUS_ERR;
	}
	return error;
}

/**
 * Update mptcp subflows state according to sent/sniffed mp_join packets.
 * Insert appropriate values retrieved from this up-to-date state in inbound
 * and outbound packets.
 */
int mptcp_subtype_mp_join(struct packet *packet_to_modify,
						struct packet *live_packet,
						struct tcp_option *tcp_opt_to_modify,
						unsigned direction)
{

	if(direction == DIRECTION_INBOUND &&
			!packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN){

		struct mp_subflow *subflow = new_subflow_inbound(packet_to_modify);
		if(!subflow)
			return STATUS_ERR;

		tcp_opt_to_modify->data.mp_join.syn.no_ack.receiver_token =
				htonl(sha1_least_32bits(mp_state.kernel_key));
		tcp_opt_to_modify->data.mp_join.syn.no_ack.sender_random_number =
				subflow->packetdrill_rand_nbr;
		tcp_opt_to_modify->data.mp_join.syn.address_id =
				subflow->packetdrill_addr_id;
	}

	else if(direction == DIRECTION_OUTBOUND &&
			packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN_ACK){

		struct mp_subflow *subflow =
				find_subflow_matching_outbound_packet(live_packet);
		struct tcp_option *live_mp_join =
				get_tcp_option(live_packet, TCPOPT_MPTCP);

		if(!subflow || !live_mp_join)
			return STATUS_ERR;

		//Update mptcp packetdrill state
		subflow->kernel_addr_id =
				live_mp_join->data.mp_join.syn.address_id;
		subflow->kernel_rand_nbr =
				live_mp_join->data.mp_join.syn.ack.sender_random_number;

		//Build key for HMAC-SHA1
		unsigned char hmac_key[16];
		unsigned long *key_b = (unsigned long*)hmac_key;
		unsigned long *key_a = (unsigned long*)&(hmac_key[8]);
		*key_b = mp_state.kernel_key;
		*key_a = mp_state.packetdrill_key;

		//Build message for HMAC-SHA1
		unsigned msg[2];
		msg[0] = subflow->kernel_rand_nbr;
		msg[1] = subflow->packetdrill_rand_nbr;

		//Update script packet mp_join option fields
		tcp_opt_to_modify->data.mp_join.syn.address_id =
				live_mp_join->data.mp_join.syn.address_id;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_random_number =
				live_mp_join->data.mp_join.syn.ack.sender_random_number;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_hmac =
				hmac_sha1_truncat_64(hmac_key,
						16,
						(char*)msg,
						8);
	}

	else if(direction == DIRECTION_INBOUND &&
			packet_to_modify->tcp->ack &&
			!packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_ACK){

		struct mp_subflow *subflow = find_subflow_matching_inbound_packet(packet_to_modify);

		if(!subflow)
			return STATUS_ERR;

		//Build HMAC-SHA1 key
		unsigned char hmac_key[16];
		unsigned long *key_a = (unsigned long*)hmac_key;
		unsigned long *key_b = (unsigned long*)&(hmac_key[8]);
		*key_a = mp_state.packetdrill_key;
		*key_b = mp_state.kernel_key;

		//Build HMAC-SHA1 message
		unsigned msg[2];
		msg[0] = subflow->packetdrill_rand_nbr;
		msg[1] = subflow->kernel_rand_nbr;

		u32 sender_hmac[5];
		hmac_sha1(hmac_key,
				16,
				(char*)msg,
				8,
				(unsigned char*)sender_hmac);

		memcpy(tcp_opt_to_modify->data.mp_join.no_syn.sender_hmac,
			   sender_hmac,
				20);
	}

	else if(direction == DIRECTION_OUTBOUND &&
			!packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN){

		struct mp_subflow *subflow = new_subflow_outbound(live_packet);

		tcp_opt_to_modify->data.mp_join.syn.address_id =
				subflow->kernel_addr_id;
		tcp_opt_to_modify->data.mp_join.syn.no_ack.sender_random_number =
				htonl(subflow->kernel_rand_nbr);
		tcp_opt_to_modify->data.mp_join.syn.no_ack.receiver_token =
				htonl(sha1_least_32bits(mp_state.kernel_key));
	}

	else if(direction == DIRECTION_INBOUND &&
			packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN_ACK){

		struct mp_subflow *subflow =
				find_subflow_matching_inbound_packet(packet_to_modify);

		if(!subflow)
			return STATUS_ERR;

		subflow->packetdrill_rand_nbr = generate_32();

		//Build key for HMAC-SHA1
		unsigned char hmac_key[16];
		unsigned long *key_a = (unsigned long*)hmac_key;
		unsigned long *key_b = (unsigned long*)&(hmac_key[8]);
		*key_a = mp_state.packetdrill_key;
		*key_b = mp_state.kernel_key;

		//Build message for HMAC-SHA1
		unsigned msg[2];
		msg[0] = subflow->packetdrill_rand_nbr;
		msg[1] = subflow->kernel_rand_nbr;

		tcp_opt_to_modify->data.mp_join.syn.address_id =
				mp_state.last_packetdrill_addr_id;
		mp_state.last_packetdrill_addr_id++;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_random_number =
				htonl(subflow->packetdrill_rand_nbr);

		tcp_opt_to_modify->data.mp_join.syn.ack.sender_hmac =
				htobe64(hmac_sha1_truncat_64(hmac_key,
						16,
						(char*)msg,
						8));
	}

	else if(direction == DIRECTION_OUTBOUND &&
			packet_to_modify->tcp->ack &&
			!packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_ACK){

		struct mp_subflow *subflow =
				find_subflow_matching_outbound_packet(packet_to_modify);

		if(!subflow)
			return STATUS_ERR;


		//Build HMAC-SHA1 key
		unsigned char hmac_key[16];
		unsigned long *key_b = (unsigned long*)hmac_key;
		unsigned long *key_a = (unsigned long*)&(hmac_key[8]);
		*key_b = mp_state.kernel_key;
		*key_a = mp_state.packetdrill_key;

		//Build HMAC-SHA1 message
		unsigned msg[2];
		msg[0] = subflow->kernel_rand_nbr;
		msg[1] = subflow->packetdrill_rand_nbr;

		u32 sender_hmac[5];
		hmac_sha1(hmac_key,
				16,
				(char*)msg,
				8,
				(unsigned char*)sender_hmac);

		memcpy(tcp_opt_to_modify->data.mp_join.no_syn.sender_hmac,
				sender_hmac,
				20);
	}

	else{
		return STATUS_ERR;
	}
	return STATUS_OK;
}

int mptcp_subtype_dss(struct packet *packet_to_modify,
						struct packet *live_packet,
						struct tcp_option *tcp_opt_to_modify,
						unsigned direction){

	if(direction == DIRECTION_INBOUND){

		if(tcp_opt_to_modify->data.dss.flag_dsn &&
				tcp_opt_to_modify->length == TCPOLEN_DSS_DSN8){

			//Computer tcp payload length
			u16 packet_total_length = packet_to_modify->ip_bytes;
			u16 tcp_header_length = packet_to_modify->tcp->doff*4;
			u16 ip_header_length = packet_to_modify->ipv4->ihl*8;
			u16 tcp_header_wo_options = 20;
			u16 tcp_payload_length = packet_total_length-ip_header_length-
					(tcp_header_length-tcp_header_wo_options);

			//Set dsn being value specified in script + initial dsn
			tcp_opt_to_modify->data.dss.dsn.data_seq_nbr_8oct =
					htobe64(mp_state.initial_dsn+
							tcp_opt_to_modify->data.dss.dsn.data_seq_nbr_8oct);

			tcp_opt_to_modify->data.dss.dsn.w_cs.data_level_length =
					htons(tcp_payload_length);

			struct mp_subflow *subflow =
					find_subflow_matching_inbound_packet(packet_to_modify);
			tcp_opt_to_modify->data.dss.dsn.w_cs.subflow_seq_nbr =
					htonl(subflow->subflow_sequence_number);
			subflow->subflow_sequence_number += tcp_payload_length;

			struct {
				u64 dsn;
				u32 ssn;
				u16 dll;
				u16 zeros;
			} buffer_checksum;

			//Compute checksum
			buffer_checksum.dsn = tcp_opt_to_modify->data.dss.dsn.data_seq_nbr_8oct;
			buffer_checksum.ssn =
					tcp_opt_to_modify->data.dss.dsn.w_cs.subflow_seq_nbr;
			buffer_checksum.dll =
					tcp_opt_to_modify->data.dss.dsn.w_cs.data_level_length;
			buffer_checksum.zeros = 0;
			//tcp_opt_to_modify->data.dss.dsn.w_cs.checksum =
			//		checksum((u16*)&buffer_checksum, sizeof(buffer_checksum));
			tcp_opt_to_modify->data.dss.dsn.w_cs.checksum = 0;
			tcp_opt_to_modify->data.dss.dsn.w_cs.checksum =
					checksum((u16*)packet_to_modify->tcp, packet_to_modify->ip_bytes - packet_ip_header_len(packet_to_modify)) +
					checksum((u16*)&buffer_checksum, sizeof(buffer_checksum));
			printf("checksum %u\n",tcp_opt_to_modify->data.dss.dsn.w_cs.checksum);
		}

		else if(tcp_opt_to_modify->data.dss.flag_dsn &&
				tcp_opt_to_modify->length == TCPOLEN_DSS_DSN8_WOCS){
			//Computer tcp payload length
			u16 packet_total_length = packet_to_modify->ip_bytes;
			u16 tcp_header_length = packet_to_modify->tcp->doff*4;
			u16 ip_header_length = packet_to_modify->ipv4->ihl*8;
			u16 tcp_header_wo_options = 20;
			u16 tcp_payload_length = packet_total_length-ip_header_length-
					(tcp_header_length-tcp_header_wo_options);

			//Set dsn being value specified in script + initial dsn

			//works for payload length = 0 or 1
			tcp_opt_to_modify->data.dss.dsn.data_seq_nbr_8oct =
								htobe64(mp_state.initial_dsn+
										tcp_opt_to_modify->data.dss.dsn.data_seq_nbr_8oct+1);

			tcp_opt_to_modify->data.dss.dsn.wo_cs.data_level_length =
					htons(tcp_payload_length);

			struct mp_subflow *subflow =
					find_subflow_matching_inbound_packet(packet_to_modify);
			tcp_opt_to_modify->data.dss.dsn.wo_cs.subflow_seq_nbr =
					htonl(subflow->subflow_sequence_number);
			subflow->subflow_sequence_number += tcp_payload_length;

		}

		if(tcp_opt_to_modify->data.dss.flag_dack){
			//TODO set DACK when receiving DSS packets
			tcp_opt_to_modify->data.dss.dack.data_ack_8oct =
					htobe64(mp_state.initial_dack +  //CPAASCH htobe64?
					tcp_opt_to_modify->data.dss.dack.data_ack_8oct); //Which initial value for mp_state.initial_dack? received dsn + received payload length
			//CPAASCH comment gérer le mélange entre dack et dsn 32 et 64 bits?
		}
	}

	else if(direction == DIRECTION_OUTBOUND){

	}

	else{
		return STATUS_ERR;
	}
	return STATUS_OK;
}

/**
 * Main function for managing mptcp packets. We have to insert appropriate
 * fields values for mptcp options according to previous state.
 *
 * Some of these values are generated randomly (packetdrill mptcp key,...)
 * others are sniffed from packets sent by the kernel (kernel mptcp key,...).
 * These values have to be inserted some mptcp script and live packets.
 */
int mptcp_insert_and_extract_opt_fields(struct packet *packet_to_modify,
		struct packet *live_packet, // could be the same as packet_to_modify
		unsigned direction)
{

	struct tcp_options_iterator tcp_opt_iter;
	struct tcp_option *tcp_opt_to_modify =
			tcp_options_begin(packet_to_modify, &tcp_opt_iter);
	int error;

	while(tcp_opt_to_modify != NULL){

		if(tcp_opt_to_modify->kind == TCPOPT_MPTCP){

			switch(tcp_opt_to_modify->data.mp_capable.subtype){

			case MP_CAPABLE_SUBTYPE:
				error = mptcp_subtype_mp_capable(packet_to_modify,
						live_packet,
						tcp_opt_to_modify,
						direction);
				break;

			case MP_JOIN_SUBTYPE:
				error = mptcp_subtype_mp_join(packet_to_modify,
						live_packet,
						tcp_opt_to_modify,
						direction);
				break;

			case DSS_SUBTYPE:
				error = mptcp_subtype_dss(packet_to_modify,
						live_packet,
						tcp_opt_to_modify,
						direction);
				break;

			default:
				error =  STATUS_ERR;
			}

			if(error)
				return STATUS_ERR;

		}
		tcp_opt_to_modify = tcp_options_next(&tcp_opt_iter, NULL);
	}

	return STATUS_OK;
}
