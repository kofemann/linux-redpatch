/*
 * zfcp device driver
 *
 * Implementation of FSF commands.
 *
 * Copyright IBM Corp. 2002, 2015
 */

#define KMSG_COMPONENT "zfcp"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/blktrace_api.h>
#include "zfcp_ext.h"
#include "zfcp_fc.h"
#include "zfcp_dbf.h"

static void zfcp_fsf_request_timeout_handler(unsigned long data)
{
	struct zfcp_adapter *adapter = (struct zfcp_adapter *) data;
	zfcp_qdio_siosl(adapter);
	zfcp_erp_adapter_reopen(adapter, ZFCP_STATUS_COMMON_ERP_FAILED,
				"fsrth_1", NULL);
}

static void zfcp_fsf_start_timer(struct zfcp_fsf_req *fsf_req,
				 unsigned long timeout)
{
	fsf_req->timer.function = zfcp_fsf_request_timeout_handler;
	fsf_req->timer.data = (unsigned long) fsf_req->adapter;
	fsf_req->timer.expires = jiffies + timeout;
	add_timer(&fsf_req->timer);
}

static void zfcp_fsf_start_erp_timer(struct zfcp_fsf_req *fsf_req)
{
	BUG_ON(!fsf_req->erp_action);
	fsf_req->timer.function = zfcp_erp_timeout_handler;
	fsf_req->timer.data = (unsigned long) fsf_req->erp_action;
	fsf_req->timer.expires = jiffies + 30 * HZ;
	add_timer(&fsf_req->timer);
}

/* association between FSF command and FSF QTCB type */
static u32 fsf_qtcb_type[] = {
	[FSF_QTCB_FCP_CMND] =             FSF_IO_COMMAND,
	[FSF_QTCB_ABORT_FCP_CMND] =       FSF_SUPPORT_COMMAND,
	[FSF_QTCB_OPEN_PORT_WITH_DID] =   FSF_SUPPORT_COMMAND,
	[FSF_QTCB_OPEN_LUN] =             FSF_SUPPORT_COMMAND,
	[FSF_QTCB_CLOSE_LUN] =            FSF_SUPPORT_COMMAND,
	[FSF_QTCB_CLOSE_PORT] =           FSF_SUPPORT_COMMAND,
	[FSF_QTCB_CLOSE_PHYSICAL_PORT] =  FSF_SUPPORT_COMMAND,
	[FSF_QTCB_SEND_ELS] =             FSF_SUPPORT_COMMAND,
	[FSF_QTCB_SEND_GENERIC] =         FSF_SUPPORT_COMMAND,
	[FSF_QTCB_EXCHANGE_CONFIG_DATA] = FSF_CONFIG_COMMAND,
	[FSF_QTCB_EXCHANGE_PORT_DATA] =   FSF_PORT_COMMAND,
	[FSF_QTCB_DOWNLOAD_CONTROL_FILE] = FSF_SUPPORT_COMMAND,
	[FSF_QTCB_UPLOAD_CONTROL_FILE] =  FSF_SUPPORT_COMMAND
};

static void zfcp_act_eval_err(struct zfcp_adapter *adapter, u32 table)
{
	u16 subtable = table >> 16;
	u16 rule = table & 0xffff;
	const char *act_type[] = { "unknown", "OS", "WWPN", "DID", "LUN" };

	if (subtable && subtable < ARRAY_SIZE(act_type))
		dev_warn(&adapter->ccw_device->dev,
			 "Access denied according to ACT rule type %s, "
			 "rule %d\n", act_type[subtable], rule);
}

static void zfcp_fsf_access_denied_port(struct zfcp_fsf_req *req,
					struct zfcp_port *port)
{
	struct fsf_qtcb_header *header = &req->qtcb->header;
	dev_warn(&req->adapter->ccw_device->dev,
		 "Access denied to port 0x%016Lx\n",
		 (unsigned long long)port->wwpn);
	zfcp_act_eval_err(req->adapter, header->fsf_status_qual.halfword[0]);
	zfcp_act_eval_err(req->adapter, header->fsf_status_qual.halfword[1]);
	zfcp_erp_port_access_denied(port, "fspad_1", req);
	req->status |= ZFCP_STATUS_FSFREQ_ERROR;
}

static void zfcp_fsf_access_denied_unit(struct zfcp_fsf_req *req,
					struct zfcp_unit *unit)
{
	struct fsf_qtcb_header *header = &req->qtcb->header;
	dev_warn(&req->adapter->ccw_device->dev,
		 "Access denied to unit 0x%016Lx on port 0x%016Lx\n",
		 (unsigned long long)unit->fcp_lun,
		 (unsigned long long)unit->port->wwpn);
	zfcp_act_eval_err(req->adapter, header->fsf_status_qual.halfword[0]);
	zfcp_act_eval_err(req->adapter, header->fsf_status_qual.halfword[1]);
	zfcp_erp_unit_access_denied(unit, "fsuad_1", req);
	req->status |= ZFCP_STATUS_FSFREQ_ERROR;
}

static void zfcp_fsf_class_not_supp(struct zfcp_fsf_req *req)
{
	dev_err(&req->adapter->ccw_device->dev, "FCP device not "
		"operational because of an unsupported FC class\n");
	zfcp_erp_adapter_shutdown(req->adapter, 0, "fscns_1", req);
	req->status |= ZFCP_STATUS_FSFREQ_ERROR;
}

/**
 * zfcp_fsf_req_free - free memory used by fsf request
 * @fsf_req: pointer to struct zfcp_fsf_req
 */
void zfcp_fsf_req_free(struct zfcp_fsf_req *req)
{
	if (likely(req->pool)) {
		if (likely(req->qtcb))
			mempool_free(req->qtcb, req->adapter->pool.qtcb_pool);
		mempool_free(req, req->pool);
		return;
	}

	if (likely(req->qtcb))
		kmem_cache_free(zfcp_data.qtcb_cache, req->qtcb);
	kfree(req);
}

static void zfcp_fsf_status_read_port_closed(struct zfcp_fsf_req *req)
{
	struct fsf_status_read_buffer *sr_buf = req->data;
	struct zfcp_adapter *adapter = req->adapter;
	struct zfcp_port *port;
	int d_id = sr_buf->d_id & ZFCP_DID_MASK;
	unsigned long flags;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	list_for_each_entry(port, &adapter->port_list_head, list)
		if (port->d_id == d_id) {
			read_unlock_irqrestore(&zfcp_data.config_lock, flags);
			zfcp_erp_port_reopen(port, 0, "fssrpc1", req);
			return;
		}
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);
}

static void zfcp_fsf_link_down_info_eval(struct zfcp_fsf_req *req, char *id,
					 struct fsf_link_down_info *link_down)
{
	struct zfcp_adapter *adapter = req->adapter;
	unsigned long flags;

	if (atomic_read(&adapter->status) & ZFCP_STATUS_ADAPTER_LINK_UNPLUGGED)
		return;

	atomic_set_mask(ZFCP_STATUS_ADAPTER_LINK_UNPLUGGED, &adapter->status);

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	zfcp_scsi_schedule_rports_block(adapter);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	if (!link_down)
		goto out;

	switch (link_down->error_code) {
	case FSF_PSQ_LINK_NO_LIGHT:
		dev_warn(&req->adapter->ccw_device->dev,
			 "There is no light signal from the local "
			 "fibre channel cable\n");
		break;
	case FSF_PSQ_LINK_WRAP_PLUG:
		dev_warn(&req->adapter->ccw_device->dev,
			 "There is a wrap plug instead of a fibre "
			 "channel cable\n");
		break;
	case FSF_PSQ_LINK_NO_FCP:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The adjacent fibre channel node does not "
			 "support FCP\n");
		break;
	case FSF_PSQ_LINK_FIRMWARE_UPDATE:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The FCP device is suspended because of a "
			 "firmware update\n");
		break;
	case FSF_PSQ_LINK_INVALID_WWPN:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The FCP device detected a WWPN that is "
			 "duplicate or not valid\n");
		break;
	case FSF_PSQ_LINK_NO_NPIV_SUPPORT:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The fibre channel fabric does not support NPIV\n");
		break;
	case FSF_PSQ_LINK_NO_FCP_RESOURCES:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The FCP adapter cannot support more NPIV ports\n");
		break;
	case FSF_PSQ_LINK_NO_FABRIC_RESOURCES:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The adjacent switch cannot support "
			 "more NPIV ports\n");
		break;
	case FSF_PSQ_LINK_FABRIC_LOGIN_UNABLE:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The FCP adapter could not log in to the "
			 "fibre channel fabric\n");
		break;
	case FSF_PSQ_LINK_WWPN_ASSIGNMENT_CORRUPTED:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The WWPN assignment file on the FCP adapter "
			 "has been damaged\n");
		break;
	case FSF_PSQ_LINK_MODE_TABLE_CURRUPTED:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The mode table on the FCP adapter "
			 "has been damaged\n");
		break;
	case FSF_PSQ_LINK_NO_WWPN_ASSIGNMENT:
		dev_warn(&req->adapter->ccw_device->dev,
			 "All NPIV ports on the FCP adapter have "
			 "been assigned\n");
		break;
	default:
		dev_warn(&req->adapter->ccw_device->dev,
			 "The link between the FCP adapter and "
			 "the FC fabric is down\n");
	}
out:
	zfcp_erp_adapter_failed(adapter, id, req);
}

static void zfcp_fsf_status_read_link_down(struct zfcp_fsf_req *req)
{
	struct fsf_status_read_buffer *sr_buf = req->data;
	struct fsf_link_down_info *ldi =
		(struct fsf_link_down_info *) &sr_buf->payload;

	switch (sr_buf->status_subtype) {
	case FSF_STATUS_READ_SUB_NO_PHYSICAL_LINK:
		zfcp_fsf_link_down_info_eval(req, "fssrld1", ldi);
		break;
	case FSF_STATUS_READ_SUB_FDISC_FAILED:
		zfcp_fsf_link_down_info_eval(req, "fssrld2", ldi);
		break;
	case FSF_STATUS_READ_SUB_FIRMWARE_UPDATE:
		zfcp_fsf_link_down_info_eval(req, "fssrld3", NULL);
	};
}

static void zfcp_fsf_status_read_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct fsf_status_read_buffer *sr_buf = req->data;

	if (req->status & ZFCP_STATUS_FSFREQ_DISMISSED) {
		zfcp_dbf_hba_fsf_unsol("dism", adapter->dbf, sr_buf);
		mempool_free(sr_buf, adapter->pool.status_read_data);
		zfcp_fsf_req_free(req);
		return;
	}

	zfcp_dbf_hba_fsf_unsol("read", adapter->dbf, sr_buf);

	switch (sr_buf->status_type) {
	case FSF_STATUS_READ_PORT_CLOSED:
		zfcp_fsf_status_read_port_closed(req);
		break;
	case FSF_STATUS_READ_INCOMING_ELS:
		zfcp_fc_incoming_els(req);
		break;
	case FSF_STATUS_READ_SENSE_DATA_AVAIL:
		break;
	case FSF_STATUS_READ_BIT_ERROR_THRESHOLD:
		dev_warn(&adapter->ccw_device->dev,
			 "The error threshold for checksum statistics "
			 "has been exceeded\n");
		zfcp_dbf_hba_berr(adapter->dbf, req);
		break;
	case FSF_STATUS_READ_LINK_DOWN:
		zfcp_fsf_status_read_link_down(req);
		zfcp_fc_enqueue_event(adapter, FCH_EVT_LINKDOWN, 0);
		break;
	case FSF_STATUS_READ_LINK_UP:
		dev_info(&adapter->ccw_device->dev,
			 "The local link has been restored\n");
		/* All ports should be marked as ready to run again */
		zfcp_erp_modify_adapter_status(adapter, "fssrh_1", NULL,
					       ZFCP_STATUS_COMMON_RUNNING,
					       ZFCP_SET);
		zfcp_erp_adapter_reopen(adapter,
					ZFCP_STATUS_ADAPTER_LINK_UNPLUGGED |
					ZFCP_STATUS_COMMON_ERP_FAILED,
					"fssrh_2", req);
		zfcp_fc_enqueue_event(adapter, FCH_EVT_LINKUP, 0);

		break;
	case FSF_STATUS_READ_NOTIFICATION_LOST:
		if (sr_buf->status_subtype & FSF_STATUS_READ_SUB_ACT_UPDATED)
			zfcp_erp_adapter_access_changed(adapter, "fssrh_3",
							req);
		if (sr_buf->status_subtype & FSF_STATUS_READ_SUB_INCOMING_ELS)
			zfcp_fc_conditional_port_scan(adapter);
		break;
	case FSF_STATUS_READ_CFDC_UPDATED:
		zfcp_erp_adapter_access_changed(adapter, "fssrh_4", req);
		break;
	case FSF_STATUS_READ_FEATURE_UPDATE_ALERT:
		adapter->adapter_features = sr_buf->payload.word[0];
		break;
	}

	mempool_free(sr_buf, adapter->pool.status_read_data);
	zfcp_fsf_req_free(req);

	atomic_inc(&adapter->stat_miss);
	queue_work(adapter->work_queue, &adapter->stat_work);
}

static void zfcp_fsf_fsfstatus_qual_eval(struct zfcp_fsf_req *req)
{
	switch (req->qtcb->header.fsf_status_qual.word[0]) {
	case FSF_SQ_FCP_RSP_AVAILABLE:
	case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
	case FSF_SQ_NO_RETRY_POSSIBLE:
	case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
		return;
	case FSF_SQ_COMMAND_ABORTED:
		break;
	case FSF_SQ_NO_RECOM:
		dev_err(&req->adapter->ccw_device->dev,
			"The FCP adapter reported a problem "
			"that cannot be recovered\n");
		zfcp_qdio_siosl(req->adapter);
		zfcp_erp_adapter_shutdown(req->adapter, 0, "fsfsqe1", req);
		break;
	}
	/* all non-return stats set FSFREQ_ERROR*/
	req->status |= ZFCP_STATUS_FSFREQ_ERROR;
}

static void zfcp_fsf_fsfstatus_eval(struct zfcp_fsf_req *req)
{
	if (unlikely(req->status & ZFCP_STATUS_FSFREQ_ERROR))
		return;

	switch (req->qtcb->header.fsf_status) {
	case FSF_UNKNOWN_COMMAND:
		dev_err(&req->adapter->ccw_device->dev,
			"The FCP adapter does not recognize the command 0x%x\n",
			req->qtcb->header.fsf_command);
		zfcp_erp_adapter_shutdown(req->adapter, 0, "fsfse_1", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		zfcp_fsf_fsfstatus_qual_eval(req);
		break;
	}
}

static void zfcp_fsf_protstatus_eval(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct fsf_qtcb *qtcb = req->qtcb;
	union fsf_prot_status_qual *psq = &qtcb->prefix.prot_status_qual;

	zfcp_dbf_hba_fsf_response(req);

	if (req->status & ZFCP_STATUS_FSFREQ_DISMISSED) {
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		return;
	}

	switch (qtcb->prefix.prot_status) {
	case FSF_PROT_GOOD:
	case FSF_PROT_FSF_STATUS_PRESENTED:
		return;
	case FSF_PROT_QTCB_VERSION_ERROR:
		dev_err(&adapter->ccw_device->dev,
			"QTCB version 0x%x not supported by FCP adapter "
			"(0x%x to 0x%x)\n", FSF_QTCB_CURRENT_VERSION,
			psq->word[0], psq->word[1]);
		zfcp_erp_adapter_shutdown(adapter, 0, "fspse_1", req);
		break;
	case FSF_PROT_ERROR_STATE:
	case FSF_PROT_SEQ_NUMB_ERROR:
		zfcp_erp_adapter_reopen(adapter, 0, "fspse_2", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_PROT_UNSUPP_QTCB_TYPE:
		dev_err(&adapter->ccw_device->dev,
			"The QTCB type is not supported by the FCP adapter\n");
		zfcp_erp_adapter_shutdown(adapter, 0, "fspse_3", req);
		break;
	case FSF_PROT_HOST_CONNECTION_INITIALIZING:
		atomic_set_mask(ZFCP_STATUS_ADAPTER_HOST_CON_INIT,
				&adapter->status);
		break;
	case FSF_PROT_DUPLICATE_REQUEST_ID:
		dev_err(&adapter->ccw_device->dev,
			"0x%Lx is an ambiguous request identifier\n",
			(unsigned long long)qtcb->bottom.support.req_handle);
		zfcp_erp_adapter_shutdown(adapter, 0, "fspse_4", req);
		break;
	case FSF_PROT_LINK_DOWN:
		zfcp_fsf_link_down_info_eval(req, "fspse_5",
					     &psq->link_down_info);
		/* FIXME: reopening adapter now? better wait for link up */
		zfcp_erp_adapter_reopen(adapter, 0, "fspse_6", req);
		break;
	case FSF_PROT_REEST_QUEUE:
		/* All ports should be marked as ready to run again */
		zfcp_erp_modify_adapter_status(adapter, "fspse_7", NULL,
					       ZFCP_STATUS_COMMON_RUNNING,
					       ZFCP_SET);
		zfcp_erp_adapter_reopen(adapter,
					ZFCP_STATUS_ADAPTER_LINK_UNPLUGGED |
					ZFCP_STATUS_COMMON_ERP_FAILED,
					"fspse_8", req);
		break;
	default:
		dev_err(&adapter->ccw_device->dev,
			"0x%x is not a valid transfer protocol status\n",
			qtcb->prefix.prot_status);
		zfcp_qdio_siosl(adapter);
		zfcp_erp_adapter_shutdown(adapter, 0, "fspse_9", req);
	}
	req->status |= ZFCP_STATUS_FSFREQ_ERROR;
}

/**
 * zfcp_fsf_req_complete - process completion of a FSF request
 * @fsf_req: The FSF request that has been completed.
 *
 * When a request has been completed either from the FCP adapter,
 * or it has been dismissed due to a queue shutdown, this function
 * is called to process the completion status and trigger further
 * events related to the FSF request.
 */
static void zfcp_fsf_req_complete(struct zfcp_fsf_req *req)
{
	if (unlikely(req->fsf_command == FSF_QTCB_UNSOLICITED_STATUS)) {
		zfcp_fsf_status_read_handler(req);
		return;
	}

	del_timer(&req->timer);
	zfcp_fsf_protstatus_eval(req);
	zfcp_fsf_fsfstatus_eval(req);
	req->handler(req);

	if (req->erp_action)
		zfcp_erp_notify(req->erp_action, 0);

	if (likely(req->status & ZFCP_STATUS_FSFREQ_CLEANUP))
		zfcp_fsf_req_free(req);
	else
		complete(&req->completion);
}

/**
 * zfcp_fsf_req_dismiss_all - dismiss all fsf requests
 * @adapter: pointer to struct zfcp_adapter
 *
 * Never ever call this without shutting down the adapter first.
 * Otherwise the adapter would continue using and corrupting s390 storage.
 * Included BUG_ON() call to ensure this is done.
 * ERP is supposed to be the only user of this function.
 */
void zfcp_fsf_req_dismiss_all(struct zfcp_adapter *adapter)
{
	struct zfcp_fsf_req *req, *tmp;
	unsigned long flags;
	LIST_HEAD(remove_queue);
	unsigned int i;

	BUG_ON(atomic_read(&adapter->status) & ZFCP_STATUS_ADAPTER_QDIOUP);
	spin_lock_irqsave(&adapter->req_list_lock, flags);
	for (i = 0; i < REQUEST_LIST_SIZE; i++)
		list_splice_init(&adapter->req_list[i], &remove_queue);
	spin_unlock_irqrestore(&adapter->req_list_lock, flags);

	list_for_each_entry_safe(req, tmp, &remove_queue, list) {
		list_del(&req->list);
		req->status |= ZFCP_STATUS_FSFREQ_DISMISSED;
		zfcp_fsf_req_complete(req);
	}
}

#define ZFCP_FSF_PORTSPEED_1GBIT	(1 <<  0)
#define ZFCP_FSF_PORTSPEED_2GBIT	(1 <<  1)
#define ZFCP_FSF_PORTSPEED_4GBIT	(1 <<  2)
#define ZFCP_FSF_PORTSPEED_10GBIT	(1 <<  3)
#define ZFCP_FSF_PORTSPEED_8GBIT	(1 <<  4)
#define ZFCP_FSF_PORTSPEED_16GBIT	(1 <<  5)
#define ZFCP_FSF_PORTSPEED_NOT_NEGOTIATED (1 << 15)

static u32 zfcp_fsf_convert_portspeed(u32 fsf_speed)
{
	u32 fdmi_speed = 0;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_1GBIT)
		fdmi_speed |= FC_PORTSPEED_1GBIT;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_2GBIT)
		fdmi_speed |= FC_PORTSPEED_2GBIT;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_4GBIT)
		fdmi_speed |= FC_PORTSPEED_4GBIT;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_10GBIT)
		fdmi_speed |= FC_PORTSPEED_10GBIT;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_8GBIT)
		fdmi_speed |= FC_PORTSPEED_8GBIT;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_16GBIT)
		fdmi_speed |= FC_PORTSPEED_16GBIT;
	if (fsf_speed & ZFCP_FSF_PORTSPEED_NOT_NEGOTIATED)
		fdmi_speed |= FC_PORTSPEED_NOT_NEGOTIATED;
	return fdmi_speed;
}

static int zfcp_fsf_exchange_config_evaluate(struct zfcp_fsf_req *req)
{
	struct fsf_qtcb_bottom_config *bottom;
	struct zfcp_adapter *adapter = req->adapter;
	struct Scsi_Host *shost = adapter->scsi_host;

	bottom = &req->qtcb->bottom.config;

	if (req->data)
		memcpy(req->data, bottom, sizeof(*bottom));

	fc_host_node_name(shost) = bottom->nport_serv_param.wwnn;
	fc_host_port_name(shost) = bottom->nport_serv_param.wwpn;
	fc_host_port_id(shost) = bottom->s_id & ZFCP_DID_MASK;
	fc_host_speed(shost) =
		zfcp_fsf_convert_portspeed(bottom->fc_link_speed);
	fc_host_supported_classes(shost) = FC_COS_CLASS2 | FC_COS_CLASS3;

	adapter->hydra_version = bottom->adapter_type;
	adapter->timer_ticks = bottom->timer_interval;

	if (fc_host_permanent_port_name(shost) == -1)
		fc_host_permanent_port_name(shost) = fc_host_port_name(shost);

	switch (bottom->fc_topology) {
	case FSF_TOPO_P2P:
		adapter->peer_d_id = bottom->peer_d_id & ZFCP_DID_MASK;
		adapter->peer_wwpn = bottom->plogi_payload.wwpn;
		adapter->peer_wwnn = bottom->plogi_payload.wwnn;
		fc_host_port_type(shost) = FC_PORTTYPE_PTP;
		break;
	case FSF_TOPO_FABRIC:
		if (bottom->connection_features & FSF_FEATURE_NPIV_MODE)
			fc_host_port_type(shost) = FC_PORTTYPE_NPIV;
		else
			fc_host_port_type(shost) = FC_PORTTYPE_NPORT;
		break;
	case FSF_TOPO_AL:
		fc_host_port_type(shost) = FC_PORTTYPE_NLPORT;
		/* fall through */
	default:
		dev_err(&adapter->ccw_device->dev,
			"Unknown or unsupported arbitrated loop "
			"fibre channel topology detected\n");
		zfcp_erp_adapter_shutdown(adapter, 0, "fsece_1", req);
		return -EIO;
	}

	zfcp_scsi_set_prot(adapter);

	return 0;
}

static void zfcp_fsf_exchange_config_data_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct fsf_qtcb *qtcb = req->qtcb;
	struct fsf_qtcb_bottom_config *bottom = &qtcb->bottom.config;
	struct Scsi_Host *shost = adapter->scsi_host;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	adapter->fsf_lic_version = bottom->lic_version;
	adapter->adapter_features = bottom->adapter_features;
	adapter->connection_features = bottom->connection_features;
	adapter->peer_wwpn = 0;
	adapter->peer_wwnn = 0;
	adapter->peer_d_id = 0;

	switch (qtcb->header.fsf_status) {
	case FSF_GOOD:
		if (zfcp_fsf_exchange_config_evaluate(req))
			return;

		if (bottom->max_qtcb_size < sizeof(struct fsf_qtcb)) {
			dev_err(&adapter->ccw_device->dev,
				"FCP adapter maximum QTCB size (%d bytes) "
				"is too small\n",
				bottom->max_qtcb_size);
			zfcp_erp_adapter_shutdown(adapter, 0, "fsecdh1", req);
			return;
		}
		atomic_set_mask(ZFCP_STATUS_ADAPTER_XCONFIG_OK,
				&adapter->status);
		break;
	case FSF_EXCHANGE_CONFIG_DATA_INCOMPLETE:
		fc_host_node_name(shost) = 0;
		fc_host_port_name(shost) = 0;
		fc_host_port_id(shost) = 0;
		fc_host_speed(shost) = FC_PORTSPEED_UNKNOWN;
		fc_host_port_type(shost) = FC_PORTTYPE_UNKNOWN;
		adapter->hydra_version = 0;

		atomic_set_mask(ZFCP_STATUS_ADAPTER_XCONFIG_OK,
				&adapter->status);

		zfcp_fsf_link_down_info_eval(req, "fsecdh2",
			&qtcb->header.fsf_status_qual.link_down_info);
		break;
	default:
		zfcp_erp_adapter_shutdown(adapter, 0, "fsecdh3", req);
		return;
	}

	if (adapter->adapter_features & FSF_FEATURE_HBAAPI_MANAGEMENT) {
		adapter->hardware_version = bottom->hardware_version;
		memcpy(fc_host_serial_number(shost), bottom->serial_number,
		       min(FC_SERIAL_NUMBER_SIZE, 17));
		EBCASC(fc_host_serial_number(shost),
		       min(FC_SERIAL_NUMBER_SIZE, 17));
	}

	if (FSF_QTCB_CURRENT_VERSION < bottom->low_qtcb_version) {
		dev_err(&adapter->ccw_device->dev,
			"The FCP adapter only supports newer "
			"control block versions\n");
		zfcp_erp_adapter_shutdown(adapter, 0, "fsecdh4", req);
		return;
	}
	if (FSF_QTCB_CURRENT_VERSION > bottom->high_qtcb_version) {
		dev_err(&adapter->ccw_device->dev,
			"The FCP adapter only supports older "
			"control block versions\n");
		zfcp_erp_adapter_shutdown(adapter, 0, "fsecdh5", req);
	}
}

static void zfcp_fsf_exchange_port_evaluate(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct fsf_qtcb_bottom_port *bottom = &req->qtcb->bottom.port;
	struct Scsi_Host *shost = adapter->scsi_host;

	if (req->data)
		memcpy(req->data, bottom, sizeof(*bottom));

	if (adapter->connection_features & FSF_FEATURE_NPIV_MODE) {
		fc_host_permanent_port_name(shost) = bottom->wwpn;
	} else
		fc_host_permanent_port_name(shost) = fc_host_port_name(shost);
	fc_host_maxframe_size(shost) = bottom->maximum_frame_size;
	fc_host_supported_speeds(shost) =
		zfcp_fsf_convert_portspeed(bottom->supported_speed);
}

static void zfcp_fsf_exchange_port_data_handler(struct zfcp_fsf_req *req)
{
	struct fsf_qtcb *qtcb = req->qtcb;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	switch (qtcb->header.fsf_status) {
	case FSF_GOOD:
		zfcp_fsf_exchange_port_evaluate(req);
		break;
	case FSF_EXCHANGE_CONFIG_DATA_INCOMPLETE:
		zfcp_fsf_exchange_port_evaluate(req);
		zfcp_fsf_link_down_info_eval(req, "fsepdh1",
			&qtcb->header.fsf_status_qual.link_down_info);
		break;
	}
}

static int zfcp_fsf_sbal_check(struct zfcp_qdio *qdio)
{
	struct zfcp_qdio_queue *req_q = &qdio->req_q;

	if (atomic_read(&req_q->count) ||
	    !(atomic_read(&qdio->adapter->status) & ZFCP_STATUS_ADAPTER_QDIOUP))
		return 1;
	return 0;
}

static int zfcp_fsf_req_sbal_get(struct zfcp_qdio *qdio)
{
	struct zfcp_adapter *adapter = qdio->adapter;
	long ret;

	ret = wait_event_interruptible_lock_bh_timeout(qdio->req_q_wq,
		       zfcp_fsf_sbal_check(qdio), qdio->req_q_lock, 5 * HZ);

	if (!(atomic_read(&qdio->adapter->status) & ZFCP_STATUS_ADAPTER_QDIOUP))
		return -EIO;

	if (ret > 0)
		return 0;

	if (!ret) {
		atomic_inc(&qdio->req_q_full);
		/* assume hanging outbound queue, try queue recovery */
		zfcp_erp_adapter_reopen(adapter, 0, "fsrsg_1", NULL);
	}

	return -EIO;
}

static struct zfcp_fsf_req *zfcp_fsf_alloc(mempool_t *pool)
{
	struct zfcp_fsf_req *req;

	if (likely(pool))
		req = mempool_alloc(pool, GFP_ATOMIC);
	else
		req = kmalloc(sizeof(*req), GFP_ATOMIC);

	if (unlikely(!req))
		return NULL;

	memset(req, 0, sizeof(*req));
	req->pool = pool;
	return req;
}

static struct fsf_qtcb *zfcp_qtcb_alloc(mempool_t *pool)
{
	struct fsf_qtcb *qtcb;

	if (likely(pool))
		qtcb = mempool_alloc(pool, GFP_ATOMIC);
	else
		qtcb = kmem_cache_alloc(zfcp_data.qtcb_cache, GFP_ATOMIC);

	if (unlikely(!qtcb))
		return NULL;

	memset(qtcb, 0, sizeof(*qtcb));
	return qtcb;
}

static struct zfcp_fsf_req *zfcp_fsf_req_create(struct zfcp_qdio *qdio,
						u32 fsf_cmd, mempool_t *pool)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio_queue *req_q = &qdio->req_q;
	struct zfcp_adapter *adapter = qdio->adapter;
	struct zfcp_fsf_req *req = zfcp_fsf_alloc(pool);

	if (unlikely(!req))
		return ERR_PTR(-ENOMEM);

	if (adapter->req_no == 0)
		adapter->req_no++;

	INIT_LIST_HEAD(&req->list);
	init_timer(&req->timer);
	init_completion(&req->completion);

	req->adapter = adapter;
	req->fsf_command = fsf_cmd;
	req->req_id = adapter->req_no;
	req->queue_req.sbal_number = 1;
	req->queue_req.sbal_first = req_q->first;
	req->queue_req.sbal_last = req_q->first;
	req->queue_req.sbale_curr = 1;

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].addr = (void *) req->req_id;
	sbale[0].eflags = 0;
	sbale[0].sflags |= SBAL_SFLAGS0_COMMAND;

	if (likely(fsf_cmd != FSF_QTCB_UNSOLICITED_STATUS)) {
		if (likely(pool))
			req->qtcb = zfcp_qtcb_alloc(adapter->pool.qtcb_pool);
		else
			req->qtcb = zfcp_qtcb_alloc(NULL);

		if (unlikely(!req->qtcb)) {
			zfcp_fsf_req_free(req);
			return ERR_PTR(-ENOMEM);
		}

		req->qtcb->prefix.req_seq_no = adapter->fsf_req_seq_no;
		req->qtcb->prefix.req_id = req->req_id;
		req->qtcb->prefix.ulp_info = 26;
		req->qtcb->prefix.qtcb_type = fsf_qtcb_type[req->fsf_command];
		req->qtcb->prefix.qtcb_version = FSF_QTCB_CURRENT_VERSION;
		req->qtcb->header.req_handle = req->req_id;
		req->qtcb->header.fsf_command = req->fsf_command;
		req->seq_no = adapter->fsf_req_seq_no;
		req->qtcb->prefix.req_seq_no = adapter->fsf_req_seq_no;
		sbale[1].addr = (void *) req->qtcb;
		sbale[1].length = sizeof(struct fsf_qtcb);
	}

	return req;
}

static int zfcp_fsf_req_send(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct zfcp_qdio *qdio = adapter->qdio;
	unsigned long	     flags;
	int		     idx;
	int		     with_qtcb = (req->qtcb != NULL);

	/* put allocated FSF request into hash table */
	spin_lock_irqsave(&adapter->req_list_lock, flags);
	idx = zfcp_reqlist_hash(req->req_id);
	list_add_tail(&req->list, &adapter->req_list[idx]);
	spin_unlock_irqrestore(&adapter->req_list_lock, flags);

	req->queue_req.qdio_outb_usage = atomic_read(&qdio->req_q.count);
	req->issued = get_clock();
	if (zfcp_qdio_send(qdio, &req->queue_req)) {
		del_timer(&req->timer);
		spin_lock_irqsave(&adapter->req_list_lock, flags);
		/* lookup request again, list might have changed */
		if (zfcp_reqlist_find_safe(adapter, req))
			zfcp_reqlist_remove(adapter, req);
		spin_unlock_irqrestore(&adapter->req_list_lock, flags);
		zfcp_erp_adapter_reopen(adapter, 0, "fsrs__1", req);
		return -EIO;
	}

	/* Don't increase for unsolicited status */
	if (with_qtcb)
		adapter->fsf_req_seq_no++;
	adapter->req_no++;

	return 0;
}

/**
 * zfcp_fsf_status_read - send status read request
 * @adapter: pointer to struct zfcp_adapter
 * @req_flags: request flags
 * Returns: 0 on success, ERROR otherwise
 */
int zfcp_fsf_status_read(struct zfcp_qdio *qdio)
{
	struct zfcp_adapter *adapter = qdio->adapter;
	struct zfcp_fsf_req *req;
	struct fsf_status_read_buffer *sr_buf;
	struct qdio_buffer_element *sbale;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_UNSOLICITED_STATUS,
				  adapter->pool.status_read_req);
	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[2].eflags |= SBAL_EFLAGS_LAST_ENTRY;
	req->queue_req.sbale_curr = 2;

	sr_buf = mempool_alloc(adapter->pool.status_read_data, GFP_ATOMIC);
	if (!sr_buf) {
		retval = -ENOMEM;
		goto failed_buf;
	}
	memset(sr_buf, 0, sizeof(*sr_buf));
	req->data = sr_buf;
	sbale = zfcp_qdio_sbale_curr(qdio, &req->queue_req);
	sbale->addr = (void *) sr_buf;
	sbale->length = sizeof(*sr_buf);

	retval = zfcp_fsf_req_send(req);
	if (retval)
		goto failed_req_send;

	goto out;

failed_req_send:
	mempool_free(sr_buf, adapter->pool.status_read_data);
failed_buf:
	zfcp_fsf_req_free(req);
	zfcp_dbf_hba_fsf_unsol("fail", adapter->dbf, NULL);
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_abort_fcp_command_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_unit *unit = req->data;
	union fsf_status_qual *fsq = &req->qtcb->header.fsf_status_qual;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	switch (req->qtcb->header.fsf_status) {
	case FSF_PORT_HANDLE_NOT_VALID:
		if (fsq->word[0] == fsq->word[1]) {
			zfcp_erp_adapter_reopen(unit->port->adapter, 0,
						"fsafch1", req);
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		}
		break;
	case FSF_LUN_HANDLE_NOT_VALID:
		if (fsq->word[0] == fsq->word[1]) {
			zfcp_erp_port_reopen(unit->port, 0, "fsafch2", req);
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		}
		break;
	case FSF_FCP_COMMAND_DOES_NOT_EXIST:
		req->status |= ZFCP_STATUS_FSFREQ_ABORTNOTNEEDED;
		break;
	case FSF_PORT_BOXED:
		zfcp_erp_port_boxed(unit->port, "fsafch3", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_LUN_BOXED:
		zfcp_erp_unit_boxed(unit, "fsafch4", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
                break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		switch (fsq->word[0]) {
		case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
			zfcp_fc_test_link(unit->port);
			/* fall through */
		case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
		}
		break;
	case FSF_GOOD:
		req->status |= ZFCP_STATUS_FSFREQ_ABORTSUCCEEDED;
		break;
	}
}

/**
 * zfcp_fsf_abort_fcp_command - abort running SCSI command
 * @old_req_id: unsigned long
 * @unit: pointer to struct zfcp_unit
 * Returns: pointer to struct zfcp_fsf_req
 */

struct zfcp_fsf_req *zfcp_fsf_abort_fcp_command(unsigned long old_req_id,
						struct zfcp_unit *unit)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *req = NULL;
	struct zfcp_qdio *qdio = unit->port->adapter->qdio;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;
	req = zfcp_fsf_req_create(qdio, FSF_QTCB_ABORT_FCP_CMND,
				  qdio->adapter->pool.scsi_abort);
	if (IS_ERR(req)) {
		req = NULL;
		goto out;
	}

	if (unlikely(!(atomic_read(&unit->status) &
		       ZFCP_STATUS_COMMON_UNBLOCKED)))
		goto out_error_free;

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->data = unit;
	req->handler = zfcp_fsf_abort_fcp_command_handler;
	req->qtcb->header.lun_handle = unit->handle;
	req->qtcb->header.port_handle = unit->port->handle;
	req->qtcb->bottom.support.req_handle = (u64) old_req_id;

	zfcp_fsf_start_timer(req, ZFCP_SCSI_ER_TIMEOUT);
	if (!zfcp_fsf_req_send(req))
		goto out;

out_error_free:
	zfcp_fsf_req_free(req);
	req = NULL;
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return req;
}

static void zfcp_fsf_send_ct_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct zfcp_send_ct *send_ct = req->data;
	struct fsf_qtcb_header *header = &req->qtcb->header;

	send_ct->status = -EINVAL;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		goto skip_fsfstatus;

	switch (header->fsf_status) {
        case FSF_GOOD:
		zfcp_dbf_san_ct_response(req);
		send_ct->status = 0;
		break;
        case FSF_SERVICE_CLASS_NOT_SUPPORTED:
		zfcp_fsf_class_not_supp(req);
		break;
        case FSF_ADAPTER_STATUS_AVAILABLE:
                switch (header->fsf_status_qual.word[0]){
                case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
                case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
                }
                break;
	case FSF_ACCESS_DENIED:
		break;
        case FSF_PORT_BOXED:
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_PORT_HANDLE_NOT_VALID:
		zfcp_erp_adapter_reopen(adapter, 0, "fsscth1", req);
		/* fall through */
	case FSF_GENERIC_COMMAND_REJECTED:
	case FSF_PAYLOAD_SIZE_MISMATCH:
	case FSF_REQUEST_SIZE_TOO_LARGE:
	case FSF_RESPONSE_SIZE_TOO_LARGE:
	case FSF_SBAL_MISMATCH:
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	}

skip_fsfstatus:
	if (send_ct->handler)
		send_ct->handler(send_ct->handler_data);
}

static void zfcp_fsf_setup_ct_els_unchained(struct qdio_buffer_element *sbale,
					    struct scatterlist *sg_req,
					    struct scatterlist *sg_resp)
{
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_WRITE_READ;
	sbale[2].addr   = sg_virt(sg_req);
	sbale[2].length = sg_req->length;
	sbale[3].addr   = sg_virt(sg_resp);
	sbale[3].length = sg_resp->length;
	sbale[3].eflags |= SBAL_EFLAGS_LAST_ENTRY;
}

static int zfcp_fsf_one_sbal(struct scatterlist *sg)
{
	return sg_is_last(sg) && sg->length <= PAGE_SIZE;
}

/**
 * zfcp_qdio_set_sbale_last - set last entry flag in current sbale
 * @qdio: pointer to struct zfcp_qdio
 * @q_req: pointer to struct zfcp_queue_req
 */
static inline
void zfcp_qdio_set_sbale_last(struct zfcp_qdio *qdio,
			      struct zfcp_queue_req *q_req)
{
	struct qdio_buffer_element *sbale;

	sbale = zfcp_qdio_sbale_curr(qdio, q_req);
	sbale->eflags |= SBAL_EFLAGS_LAST_ENTRY;
}

/**
 * zfcp_qdio_skip_to_last_sbale - skip to last sbale in sbal
 * @q_req: The current zfcp_queue_req
 */
static inline
void zfcp_qdio_skip_to_last_sbale(struct zfcp_qdio *qdio,
				  struct zfcp_queue_req *q_req)
{
	q_req->sbale_curr = qdio->max_sbale_per_sbal - 1;
}

/**
 * zfcp_qdio_set_data_div - set data division count
 * @qdio: pointer to struct zfcp_qdio
 * @q_req: The current zfcp_queue_req
 * @count: The data division count
 */
static inline
void zfcp_qdio_set_data_div(struct zfcp_qdio *qdio,
			    struct zfcp_queue_req *q_req, u32 count)
{
	struct qdio_buffer_element *sbale;

	sbale = qdio->req_q.sbal[q_req->sbal_first]->element;
	sbale->length = count;
}

/**
 * zfcp_qdio_sbale_count - count sbale used
 * @sg: pointer to struct scatterlist
 */
static inline
unsigned int zfcp_qdio_sbale_count(struct scatterlist *sg)
{
	unsigned int count = 0;

	for (; sg; sg = sg_next(sg))
		count++;

	return count;
}

/**
 * zfcp_qdio_set_scount - set SBAL count value
 * @qdio: pointer to struct zfcp_qdio
 * @q_req: The current zfcp_queue_req
 */
static inline
void zfcp_qdio_set_scount(struct zfcp_qdio *qdio, struct zfcp_queue_req *q_req)
{
	struct qdio_buffer_element *sbale;

	sbale = qdio->req_q.sbal[q_req->sbal_first]->element;
	sbale->scount = q_req->sbal_number - 1;
}

/**
 * zfcp_qdio_real_bytes - count bytes used
 * @sg: pointer to struct scatterlist
 */
static inline
unsigned int zfcp_qdio_real_bytes(struct scatterlist *sg)
{
	unsigned int real_bytes = 0;

	for (; sg; sg = sg_next(sg))
		real_bytes += sg->length;

	return real_bytes;
}

static int zfcp_fsf_setup_ct_els_sbals(struct zfcp_fsf_req *req,
				       struct scatterlist *sg_req,
				       struct scatterlist *sg_resp,
				       int max_sbals)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct qdio_buffer_element *sbale = zfcp_qdio_sbale_req(adapter->qdio,
							       &req->queue_req);
	struct zfcp_qdio *qdio = adapter->qdio;
	struct fsf_qtcb *qtcb = req->qtcb;
	u32 feat = adapter->adapter_features;

	if (zfcp_adapter_multi_buffer_active(adapter)) {
		if (zfcp_qdio_sbals_from_sg(qdio, &req->queue_req,
					    SBAL_SFLAGS0_TYPE_WRITE_READ,
					    sg_req, max_sbals))
			return -EIO;
		qtcb->bottom.support.req_buf_length =
			zfcp_qdio_real_bytes(sg_req);
		if (zfcp_qdio_sbals_from_sg(qdio, &req->queue_req,
					    SBAL_SFLAGS0_TYPE_WRITE_READ,
					    sg_resp, max_sbals))
			return -EIO;
		qtcb->bottom.support.resp_buf_length =
			zfcp_qdio_real_bytes(sg_resp);

		zfcp_qdio_set_data_div(qdio, &req->queue_req,
				       zfcp_qdio_sbale_count(sg_req));
		zfcp_qdio_set_sbale_last(qdio, &req->queue_req);
		zfcp_qdio_set_scount(qdio, &req->queue_req);
		return 0;
	}

	/* use single, unchained SBAL if it can hold the request */
	if (zfcp_fsf_one_sbal(sg_req) && zfcp_fsf_one_sbal(sg_resp)) {
		zfcp_fsf_setup_ct_els_unchained(sbale, sg_req, sg_resp);
		return 0;
	}

	if (!(feat & FSF_FEATURE_ELS_CT_CHAINED_SBALS))
		return -EOPNOTSUPP;

	if (zfcp_qdio_sbals_from_sg(qdio, &req->queue_req,
				    SBAL_SFLAGS0_TYPE_WRITE_READ,
				    sg_req, max_sbals))
		return -EIO;

	qtcb->bottom.support.req_buf_length = zfcp_qdio_real_bytes(sg_req);

	zfcp_qdio_set_sbale_last(qdio, &req->queue_req);
	zfcp_qdio_skip_to_last_sbale(qdio, &req->queue_req);

	if (zfcp_qdio_sbals_from_sg(qdio, &req->queue_req,
				    SBAL_SFLAGS0_TYPE_WRITE_READ,
				    sg_resp, max_sbals))
		return -EIO;

	qtcb->bottom.support.resp_buf_length = zfcp_qdio_real_bytes(sg_resp);

	zfcp_qdio_set_sbale_last(qdio, &req->queue_req);

	return 0;
}

static int zfcp_fsf_setup_ct_els(struct zfcp_fsf_req *req,
				 struct scatterlist *sg_req,
				 struct scatterlist *sg_resp,
				 int max_sbals, unsigned int timeout)
{
	int ret;

	ret = zfcp_fsf_setup_ct_els_sbals(req, sg_req, sg_resp, max_sbals);
	if (ret)
		return ret;

	/* common settings for ct/gs and els requests */
	req->qtcb->bottom.support.service_class = FSF_CLASS_3;
	if (timeout > 255)
		timeout = 255; /* max value accepted by hardware */
	req->qtcb->bottom.support.timeout = timeout;
	zfcp_fsf_start_timer(req, (timeout + 10) * HZ);

	return 0;
}

/**
 * zfcp_fsf_send_ct - initiate a Generic Service request (FC-GS)
 * @ct: pointer to struct zfcp_send_ct with data for request
 * @pool: if non-null this mempool is used to allocate struct zfcp_fsf_req
 */
int zfcp_fsf_send_ct(struct zfcp_send_ct *ct, mempool_t *pool,
		     unsigned int timeout)
{
	struct zfcp_wka_port *wka_port = ct->wka_port;
	struct zfcp_qdio *qdio = wka_port->adapter->qdio;
	struct zfcp_fsf_req *req;
	int ret = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_SEND_GENERIC, pool);

	if (IS_ERR(req)) {
		ret = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	ret = zfcp_fsf_setup_ct_els(req, ct->req, ct->resp,
				    FSF_MAX_SBALS_PER_REQ, timeout);
	if (ret)
		goto failed_send;

	req->handler = zfcp_fsf_send_ct_handler;
	req->qtcb->header.port_handle = wka_port->handle;
	req->data = ct;

	zfcp_dbf_san_ct_request(req);

	ret = zfcp_fsf_req_send(req);
	if (ret)
		goto failed_send;

	goto out;

failed_send:
	zfcp_fsf_req_free(req);
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return ret;
}

static void zfcp_fsf_send_els_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_send_els *send_els = req->data;
	struct zfcp_port *port = send_els->port;
	struct fsf_qtcb_header *header = &req->qtcb->header;

	send_els->status = -EINVAL;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		goto skip_fsfstatus;

	switch (header->fsf_status) {
	case FSF_GOOD:
		zfcp_dbf_san_els_response(req);
		send_els->status = 0;
		break;
	case FSF_SERVICE_CLASS_NOT_SUPPORTED:
		zfcp_fsf_class_not_supp(req);
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		switch (header->fsf_status_qual.word[0]){
		case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
			if (port && (send_els->ls_code != ZFCP_LS_ADISC))
				zfcp_fc_test_link(port);
			/*fall through */
		case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
		case FSF_SQ_RETRY_IF_POSSIBLE:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
		}
		break;
	case FSF_ELS_COMMAND_REJECTED:
	case FSF_PAYLOAD_SIZE_MISMATCH:
	case FSF_REQUEST_SIZE_TOO_LARGE:
	case FSF_RESPONSE_SIZE_TOO_LARGE:
		break;
	case FSF_ACCESS_DENIED:
		if (port)
			zfcp_fsf_access_denied_port(req, port);
		break;
	case FSF_SBAL_MISMATCH:
		/* should never occure, avoided in zfcp_fsf_send_els */
		/* fall through */
	default:
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	}
skip_fsfstatus:
	if (send_els->handler)
		send_els->handler(send_els->handler_data);
}

/**
 * zfcp_fsf_send_els - initiate an ELS command (FC-FS)
 * @els: pointer to struct zfcp_send_els with data for the command
 */
int zfcp_fsf_send_els(struct zfcp_send_els *els, unsigned int timeout)
{
	struct zfcp_fsf_req *req;
	struct zfcp_qdio *qdio = els->adapter->qdio;
	int ret = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_SEND_ELS, NULL);

	if (IS_ERR(req)) {
		ret = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;

#if 1 /* FIXME */
	if (!zfcp_adapter_multi_buffer_active(els->adapter))
		zfcp_qdio_sbal_limit(qdio, &req->queue_req, 2);
#endif

	ret = zfcp_fsf_setup_ct_els(req, els->req, els->resp, 2, timeout);

	if (ret)
		goto failed_send;

	req->qtcb->bottom.support.d_id = els->d_id;
	req->handler = zfcp_fsf_send_els_handler;
	req->data = els;

	zfcp_dbf_san_els_request(req);

	ret = zfcp_fsf_req_send(req);
	if (ret)
		goto failed_send;

	goto out;

failed_send:
	zfcp_fsf_req_free(req);
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return ret;
}

int zfcp_fsf_exchange_config_data(struct zfcp_erp_action *erp_action)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *req;
	struct zfcp_qdio *qdio = erp_action->adapter->qdio;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_EXCHANGE_CONFIG_DATA,
				  qdio->adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->qtcb->bottom.config.feature_selection =
			FSF_FEATURE_CFDC |
			FSF_FEATURE_LUN_SHARING |
			FSF_FEATURE_NOTIFICATION_LOST |
			FSF_FEATURE_UPDATE_ALERT;
	req->erp_action = erp_action;
	req->handler = zfcp_fsf_exchange_config_data_handler;
	erp_action->fsf_req = req;

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

int zfcp_fsf_exchange_config_data_sync(struct zfcp_qdio *qdio,
				       struct fsf_qtcb_bottom_config *data)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *req = NULL;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out_unlock;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_EXCHANGE_CONFIG_DATA, NULL);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out_unlock;
	}

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;
	req->handler = zfcp_fsf_exchange_config_data_handler;

	req->qtcb->bottom.config.feature_selection =
			FSF_FEATURE_CFDC |
			FSF_FEATURE_LUN_SHARING |
			FSF_FEATURE_NOTIFICATION_LOST |
			FSF_FEATURE_UPDATE_ALERT;

	if (data)
		req->data = data;

	zfcp_fsf_start_timer(req, ZFCP_FSF_REQUEST_TIMEOUT);
	retval = zfcp_fsf_req_send(req);
	spin_unlock_bh(&qdio->req_q_lock);
	if (!retval)
		wait_for_completion(&req->completion);

	zfcp_fsf_req_free(req);
	return retval;

out_unlock:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

/**
 * zfcp_fsf_exchange_port_data - request information about local port
 * @erp_action: ERP action for the adapter for which port data is requested
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_exchange_port_data(struct zfcp_erp_action *erp_action)
{
	struct zfcp_qdio *qdio = erp_action->adapter->qdio;
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	if (!(qdio->adapter->adapter_features & FSF_FEATURE_HBAAPI_MANAGEMENT))
		return -EOPNOTSUPP;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_EXCHANGE_PORT_DATA,
				  qdio->adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->handler = zfcp_fsf_exchange_port_data_handler;
	req->erp_action = erp_action;
	erp_action->fsf_req = req;

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

/**
 * zfcp_fsf_exchange_port_data_sync - request information about local port
 * @qdio: pointer to struct zfcp_qdio
 * @data: pointer to struct fsf_qtcb_bottom_port
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_exchange_port_data_sync(struct zfcp_qdio *qdio,
				     struct fsf_qtcb_bottom_port *data)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *req = NULL;
	int retval = -EIO;

	if (!(qdio->adapter->adapter_features & FSF_FEATURE_HBAAPI_MANAGEMENT))
		return -EOPNOTSUPP;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out_unlock;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_EXCHANGE_PORT_DATA, NULL);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out_unlock;
	}

	if (data)
		req->data = data;

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->handler = zfcp_fsf_exchange_port_data_handler;
	zfcp_fsf_start_timer(req, ZFCP_FSF_REQUEST_TIMEOUT);
	retval = zfcp_fsf_req_send(req);
	spin_unlock_bh(&qdio->req_q_lock);

	if (!retval)
		wait_for_completion(&req->completion);

	zfcp_fsf_req_free(req);

	return retval;

out_unlock:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_open_port_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_port *port = req->data;
	struct fsf_qtcb_header *header = &req->qtcb->header;
	struct fsf_plogi *plogi;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		goto out;

	switch (header->fsf_status) {
	case FSF_PORT_ALREADY_OPEN:
		break;
	case FSF_ACCESS_DENIED:
		zfcp_fsf_access_denied_port(req, port);
		break;
	case FSF_MAXIMUM_NUMBER_OF_PORTS_EXCEEDED:
		dev_warn(&req->adapter->ccw_device->dev,
			 "Not enough FCP adapter resources to open "
			 "remote port 0x%016Lx\n",
			 (unsigned long long)port->wwpn);
		zfcp_erp_port_failed(port, "fsoph_1", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		switch (header->fsf_status_qual.word[0]) {
		case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
		case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
		case FSF_SQ_NO_RETRY_POSSIBLE:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
		}
		break;
	case FSF_GOOD:
		port->handle = header->port_handle;
		atomic_set_mask(ZFCP_STATUS_COMMON_OPEN |
				ZFCP_STATUS_PORT_PHYS_OPEN, &port->status);
		atomic_clear_mask(ZFCP_STATUS_COMMON_ACCESS_DENIED |
		                  ZFCP_STATUS_COMMON_ACCESS_BOXED,
		                  &port->status);
		/* check whether D_ID has changed during open */
		/*
		 * FIXME: This check is not airtight, as the FCP channel does
		 * not monitor closures of target port connections caused on
		 * the remote side. Thus, they might miss out on invalidating
		 * locally cached WWPNs (and other N_Port parameters) of gone
		 * target ports. So, our heroic attempt to make things safe
		 * could be undermined by 'open port' response data tagged with
		 * obsolete WWPNs. Another reason to monitor potential
		 * connection closures ourself at least (by interpreting
		 * incoming ELS' and unsolicited status). It just crosses my
		 * mind that one should be able to cross-check by means of
		 * another GID_PN straight after a port has been opened.
		 * Alternately, an ADISC/PDISC ELS should suffice, as well.
		 */
		plogi = (struct fsf_plogi *) req->qtcb->bottom.support.els;
		if (req->qtcb->bottom.support.els1_length >=
		    FSF_PLOGI_MIN_LEN) {
			if (plogi->serv_param.wwpn != port->wwpn) {
				port->d_id = 0;
				dev_warn(&port->adapter->ccw_device->dev,
					 "A port opened with WWPN 0x%016Lx "
					 "returned data that identifies it as "
					 "WWPN 0x%016Lx\n",
					 (unsigned long long) port->wwpn,
					 (unsigned long long)
					  plogi->serv_param.wwpn);
			} else {
				port->wwnn = plogi->serv_param.wwnn;
				zfcp_fc_plogi_evaluate(port, plogi);
			}
		}
		break;
	case FSF_UNKNOWN_OP_SUBTYPE:
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	}

out:
	zfcp_port_put(port);
}

/**
 * zfcp_fsf_open_port - create and send open port request
 * @erp_action: pointer to struct zfcp_erp_action
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_open_port(struct zfcp_erp_action *erp_action)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = erp_action->adapter->qdio;
	struct zfcp_port *port = erp_action->port;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_OPEN_PORT_WITH_DID,
				  qdio->adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->handler = zfcp_fsf_open_port_handler;
	req->qtcb->bottom.support.d_id = port->d_id;
	req->data = port;
	req->erp_action = erp_action;
	erp_action->fsf_req = req;
	zfcp_port_get(port);

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
		zfcp_port_put(port);
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_close_port_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_port *port = req->data;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	switch (req->qtcb->header.fsf_status) {
	case FSF_PORT_HANDLE_NOT_VALID:
		zfcp_erp_adapter_reopen(port->adapter, 0, "fscph_1", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		break;
	case FSF_GOOD:
		zfcp_erp_modify_port_status(port, "fscph_2", req,
					    ZFCP_STATUS_COMMON_OPEN,
					    ZFCP_CLEAR);
		break;
	}
}

/**
 * zfcp_fsf_close_port - create and send close port request
 * @erp_action: pointer to struct zfcp_erp_action
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_close_port(struct zfcp_erp_action *erp_action)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = erp_action->adapter->qdio;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_CLOSE_PORT,
				  qdio->adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->handler = zfcp_fsf_close_port_handler;
	req->data = erp_action->port;
	req->erp_action = erp_action;
	req->qtcb->header.port_handle = erp_action->port->handle;
	erp_action->fsf_req = req;

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_open_wka_port_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_wka_port *wka_port = req->data;
	struct fsf_qtcb_header *header = &req->qtcb->header;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR) {
		wka_port->status = ZFCP_WKA_PORT_OFFLINE;
		goto out;
	}

	switch (header->fsf_status) {
	case FSF_MAXIMUM_NUMBER_OF_PORTS_EXCEEDED:
		dev_warn(&req->adapter->ccw_device->dev,
			 "Opening WKA port 0x%x failed\n", wka_port->d_id);
		/* fall through */
	case FSF_ADAPTER_STATUS_AVAILABLE:
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		/* fall through */
	case FSF_ACCESS_DENIED:
		wka_port->status = ZFCP_WKA_PORT_OFFLINE;
		break;
	case FSF_GOOD:
		wka_port->handle = header->port_handle;
		/* fall through */
	case FSF_PORT_ALREADY_OPEN:
		wka_port->status = ZFCP_WKA_PORT_ONLINE;
	}
out:
	wake_up(&wka_port->completion_wq);
}

/**
 * zfcp_fsf_open_wka_port - create and send open wka-port request
 * @wka_port: pointer to struct zfcp_wka_port
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_open_wka_port(struct zfcp_wka_port *wka_port)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = wka_port->adapter->qdio;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_OPEN_PORT_WITH_DID,
				  qdio->adapter->pool.erp_req);

	if (unlikely(IS_ERR(req))) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->handler = zfcp_fsf_open_wka_port_handler;
	req->qtcb->bottom.support.d_id = wka_port->d_id;
	req->data = wka_port;

	zfcp_fsf_start_timer(req, ZFCP_FSF_REQUEST_TIMEOUT);
	retval = zfcp_fsf_req_send(req);
	if (retval)
		zfcp_fsf_req_free(req);
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_close_wka_port_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_wka_port *wka_port = req->data;

	if (req->qtcb->header.fsf_status == FSF_PORT_HANDLE_NOT_VALID) {
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		zfcp_erp_adapter_reopen(wka_port->adapter, 0, "fscwph1", req);
	}

	wka_port->status = ZFCP_WKA_PORT_OFFLINE;
	wake_up(&wka_port->completion_wq);
}

/**
 * zfcp_fsf_close_wka_port - create and send close wka port request
 * @erp_action: pointer to struct zfcp_erp_action
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_close_wka_port(struct zfcp_wka_port *wka_port)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = wka_port->adapter->qdio;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_CLOSE_PORT,
				  qdio->adapter->pool.erp_req);

	if (unlikely(IS_ERR(req))) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->handler = zfcp_fsf_close_wka_port_handler;
	req->data = wka_port;
	req->qtcb->header.port_handle = wka_port->handle;

	zfcp_fsf_start_timer(req, ZFCP_FSF_REQUEST_TIMEOUT);
	retval = zfcp_fsf_req_send(req);
	if (retval)
		zfcp_fsf_req_free(req);
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_close_physical_port_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_port *port = req->data;
	struct fsf_qtcb_header *header = &req->qtcb->header;
	struct zfcp_unit *unit;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	switch (header->fsf_status) {
	case FSF_PORT_HANDLE_NOT_VALID:
		zfcp_erp_adapter_reopen(port->adapter, 0, "fscpph1", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ACCESS_DENIED:
		zfcp_fsf_access_denied_port(req, port);
		break;
	case FSF_PORT_BOXED:
		/* can't use generic zfcp_erp_modify_port_status because
		 * ZFCP_STATUS_COMMON_OPEN must not be reset for the port */
		atomic_clear_mask(ZFCP_STATUS_PORT_PHYS_OPEN, &port->status);
		list_for_each_entry(unit, &port->unit_list_head, list)
			atomic_clear_mask(ZFCP_STATUS_COMMON_OPEN,
					  &unit->status);
		zfcp_erp_port_boxed(port, "fscpph2", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		switch (header->fsf_status_qual.word[0]) {
		case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
			/* fall through */
		case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
		}
		break;
	case FSF_GOOD:
		/* can't use generic zfcp_erp_modify_port_status because
		 * ZFCP_STATUS_COMMON_OPEN must not be reset for the port
		 */
		atomic_clear_mask(ZFCP_STATUS_PORT_PHYS_OPEN, &port->status);
		list_for_each_entry(unit, &port->unit_list_head, list)
			atomic_clear_mask(ZFCP_STATUS_COMMON_OPEN,
					  &unit->status);
		break;
	}
}

/**
 * zfcp_fsf_close_physical_port - close physical port
 * @erp_action: pointer to struct zfcp_erp_action
 * Returns: 0 on success
 */
int zfcp_fsf_close_physical_port(struct zfcp_erp_action *erp_action)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = erp_action->adapter->qdio;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_CLOSE_PHYSICAL_PORT,
				  qdio->adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->data = erp_action->port;
	req->qtcb->header.port_handle = erp_action->port->handle;
	req->erp_action = erp_action;
	req->handler = zfcp_fsf_close_physical_port_handler;
	erp_action->fsf_req = req;

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_open_unit_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_adapter *adapter = req->adapter;
	struct zfcp_unit *unit = req->data;
	struct fsf_qtcb_header *header = &req->qtcb->header;
	struct fsf_qtcb_bottom_support *bottom = &req->qtcb->bottom.support;
	struct fsf_queue_designator *queue_designator =
				&header->fsf_status_qual.fsf_queue_designator;
	int exclusive, readwrite;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	atomic_clear_mask(ZFCP_STATUS_COMMON_ACCESS_DENIED |
			  ZFCP_STATUS_COMMON_ACCESS_BOXED |
			  ZFCP_STATUS_UNIT_SHARED |
			  ZFCP_STATUS_UNIT_READONLY,
			  &unit->status);

	switch (header->fsf_status) {

	case FSF_PORT_HANDLE_NOT_VALID:
		zfcp_erp_adapter_reopen(unit->port->adapter, 0, "fsouh_1", req);
		/* fall through */
	case FSF_LUN_ALREADY_OPEN:
		break;
	case FSF_ACCESS_DENIED:
		zfcp_fsf_access_denied_unit(req, unit);
		atomic_clear_mask(ZFCP_STATUS_UNIT_SHARED, &unit->status);
		atomic_clear_mask(ZFCP_STATUS_UNIT_READONLY, &unit->status);
		break;
	case FSF_PORT_BOXED:
		zfcp_erp_port_boxed(unit->port, "fsouh_2", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_LUN_SHARING_VIOLATION:
		if (header->fsf_status_qual.word[0])
			dev_warn(&adapter->ccw_device->dev,
				 "LUN 0x%Lx on port 0x%Lx is already in "
				 "use by CSS%d, MIF Image ID %x\n",
				 (unsigned long long)unit->fcp_lun,
				 (unsigned long long)unit->port->wwpn,
				 queue_designator->cssid,
				 queue_designator->hla);
		else
			zfcp_act_eval_err(adapter,
					  header->fsf_status_qual.word[2]);
		zfcp_erp_unit_access_denied(unit, "fsouh_3", req);
		atomic_clear_mask(ZFCP_STATUS_UNIT_SHARED, &unit->status);
		atomic_clear_mask(ZFCP_STATUS_UNIT_READONLY, &unit->status);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_MAXIMUM_NUMBER_OF_LUNS_EXCEEDED:
		dev_warn(&adapter->ccw_device->dev,
			 "No handle is available for LUN "
			 "0x%016Lx on port 0x%016Lx\n",
			 (unsigned long long)unit->fcp_lun,
			 (unsigned long long)unit->port->wwpn);
		zfcp_erp_unit_failed(unit, "fsouh_4", req);
		/* fall through */
	case FSF_INVALID_COMMAND_OPTION:
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		switch (header->fsf_status_qual.word[0]) {
		case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
			zfcp_fc_test_link(unit->port);
			/* fall through */
		case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
		}
		break;

	case FSF_GOOD:
		unit->handle = header->lun_handle;
		atomic_set_mask(ZFCP_STATUS_COMMON_OPEN, &unit->status);

		if (!(adapter->connection_features & FSF_FEATURE_NPIV_MODE) &&
		    (adapter->adapter_features & FSF_FEATURE_LUN_SHARING) &&
		    !zfcp_ccw_priv_sch(adapter)) {
			exclusive = (bottom->lun_access_info &
					FSF_UNIT_ACCESS_EXCLUSIVE);
			readwrite = (bottom->lun_access_info &
					FSF_UNIT_ACCESS_OUTBOUND_TRANSFER);

			if (!exclusive)
		                atomic_set_mask(ZFCP_STATUS_UNIT_SHARED,
						&unit->status);

			if (!readwrite) {
                		atomic_set_mask(ZFCP_STATUS_UNIT_READONLY,
						&unit->status);
				dev_info(&adapter->ccw_device->dev,
					 "SCSI device at LUN 0x%016Lx on port "
					 "0x%016Lx opened read-only\n",
					 (unsigned long long)unit->fcp_lun,
					 (unsigned long long)unit->port->wwpn);
        		}

        		if (exclusive && !readwrite) {
				dev_err(&adapter->ccw_device->dev,
					"Exclusive read-only access not "
					"supported (unit 0x%016Lx, "
					"port 0x%016Lx)\n",
					(unsigned long long)unit->fcp_lun,
					(unsigned long long)unit->port->wwpn);
				zfcp_erp_unit_failed(unit, "fsouh_5", req);
				req->status |= ZFCP_STATUS_FSFREQ_ERROR;
				zfcp_erp_unit_shutdown(unit, 0, "fsouh_6", req);
        		} else if (!exclusive && readwrite) {
				dev_err(&adapter->ccw_device->dev,
					"Shared read-write access not "
					"supported (unit 0x%016Lx, port "
					"0x%016Lx)\n",
					(unsigned long long)unit->fcp_lun,
					(unsigned long long)unit->port->wwpn);
				zfcp_erp_unit_failed(unit, "fsouh_7", req);
				req->status |= ZFCP_STATUS_FSFREQ_ERROR;
				zfcp_erp_unit_shutdown(unit, 0, "fsouh_8", req);
        		}
		}
		break;
	}
}

/**
 * zfcp_fsf_open_unit - open unit
 * @erp_action: pointer to struct zfcp_erp_action
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_open_unit(struct zfcp_erp_action *erp_action)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_adapter *adapter = erp_action->adapter;
	struct zfcp_qdio *qdio = adapter->qdio;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_OPEN_LUN,
				  adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->qtcb->header.port_handle = erp_action->port->handle;
	req->qtcb->bottom.support.fcp_lun = erp_action->unit->fcp_lun;
	req->handler = zfcp_fsf_open_unit_handler;
	req->data = erp_action->unit;
	req->erp_action = erp_action;
	erp_action->fsf_req = req;

	if (!(adapter->connection_features & FSF_FEATURE_NPIV_MODE))
		req->qtcb->bottom.support.option = FSF_OPEN_LUN_SUPPRESS_BOXING;

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_close_unit_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_unit *unit = req->data;

	if (req->status & ZFCP_STATUS_FSFREQ_ERROR)
		return;

	switch (req->qtcb->header.fsf_status) {
	case FSF_PORT_HANDLE_NOT_VALID:
		zfcp_erp_adapter_reopen(unit->port->adapter, 0, "fscuh_1", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_LUN_HANDLE_NOT_VALID:
		zfcp_erp_port_reopen(unit->port, 0, "fscuh_2", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_PORT_BOXED:
		zfcp_erp_port_boxed(unit->port, "fscuh_3", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		switch (req->qtcb->header.fsf_status_qual.word[0]) {
		case FSF_SQ_INVOKE_LINK_TEST_PROCEDURE:
			zfcp_fc_test_link(unit->port);
			/* fall through */
		case FSF_SQ_ULP_DEPENDENT_ERP_REQUIRED:
			req->status |= ZFCP_STATUS_FSFREQ_ERROR;
			break;
		}
		break;
	case FSF_GOOD:
		atomic_clear_mask(ZFCP_STATUS_COMMON_OPEN, &unit->status);
		break;
	}
}

/**
 * zfcp_fsf_close_unit - close zfcp unit
 * @erp_action: pointer to struct zfcp_unit
 * Returns: 0 on success, error otherwise
 */
int zfcp_fsf_close_unit(struct zfcp_erp_action *erp_action)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = erp_action->adapter->qdio;
	struct zfcp_fsf_req *req;
	int retval = -EIO;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_CLOSE_LUN,
				  qdio->adapter->pool.erp_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_READ;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	req->qtcb->header.port_handle = erp_action->port->handle;
	req->qtcb->header.lun_handle = erp_action->unit->handle;
	req->handler = zfcp_fsf_close_unit_handler;
	req->data = erp_action->unit;
	req->erp_action = erp_action;
	erp_action->fsf_req = req;

	zfcp_fsf_start_erp_timer(req);
	retval = zfcp_fsf_req_send(req);
	if (retval) {
		zfcp_fsf_req_free(req);
		erp_action->fsf_req = NULL;
	}
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return retval;
}

static void zfcp_fsf_update_lat(struct fsf_latency_record *lat_rec, u32 lat)
{
	lat_rec->sum += lat;
	lat_rec->min = min(lat_rec->min, lat);
	lat_rec->max = max(lat_rec->max, lat);
}

static void zfcp_fsf_req_latency(struct zfcp_fsf_req *req)
{
	struct fsf_qual_latency_info *lat_inf;
	struct latency_cont *lat;
	struct zfcp_unit *unit = req->unit;

	lat_inf = &req->qtcb->prefix.prot_status_qual.latency_info;

	switch (req->qtcb->bottom.io.data_direction) {
	case FSF_DATADIR_DIF_READ_STRIP:
	case FSF_DATADIR_DIF_READ_CONVERT:
	case FSF_DATADIR_READ:
		lat = &unit->latencies.read;
		break;
	case FSF_DATADIR_DIF_WRITE_INSERT:
	case FSF_DATADIR_DIF_WRITE_CONVERT:
	case FSF_DATADIR_WRITE:
		lat = &unit->latencies.write;
		break;
	case FSF_DATADIR_CMND:
		lat = &unit->latencies.cmd;
		break;
	default:
		return;
	}

	spin_lock(&unit->latencies.lock);
	zfcp_fsf_update_lat(&lat->channel, lat_inf->channel_lat);
	zfcp_fsf_update_lat(&lat->fabric, lat_inf->fabric_lat);
	lat->counter++;
	spin_unlock(&unit->latencies.lock);
}

#ifdef CONFIG_BLK_DEV_IO_TRACE
static void zfcp_fsf_trace_latency(struct zfcp_fsf_req *fsf_req)
{
	struct fsf_qual_latency_info *lat_inf;
	struct scsi_cmnd *scsi_cmnd = (struct scsi_cmnd *)fsf_req->data;
	struct request *req = scsi_cmnd->request;
	struct zfcp_blk_drv_data trace;
	int ticks = fsf_req->adapter->timer_ticks;

	trace.flags = 0;
	trace.magic = ZFCP_BLK_DRV_DATA_MAGIC;
	if (fsf_req->adapter->adapter_features & FSF_FEATURE_MEASUREMENT_DATA) {
		trace.flags |= ZFCP_BLK_LAT_VALID;
		lat_inf = &fsf_req->qtcb->prefix.prot_status_qual.latency_info;
		trace.channel_lat = lat_inf->channel_lat * ticks;
		trace.fabric_lat = lat_inf->fabric_lat * ticks;
	}
	if (fsf_req->status & ZFCP_STATUS_FSFREQ_ERROR)
		trace.flags |= ZFCP_BLK_REQ_ERROR;
	trace.inb_usage = fsf_req->queue_req.qdio_inb_usage;
	trace.outb_usage = fsf_req->queue_req.qdio_outb_usage;

	blk_add_driver_data(req->q, req, &trace, sizeof(trace));
}
#else
static inline void zfcp_fsf_trace_latency(struct zfcp_fsf_req *fsf_req)
{
}
#endif

static void zfcp_fsf_send_fcp_command_task_handler(struct zfcp_fsf_req *req)
{
	struct scsi_cmnd *scpnt;
	struct fcp_resp_with_ext *fcp_rsp;
	unsigned long flags;

	read_lock_irqsave(&req->adapter->abort_lock, flags);

	scpnt = req->data;
	if (unlikely(!scpnt)) {
		read_unlock_irqrestore(&req->adapter->abort_lock, flags);
		return;
	}

	if (unlikely(req->status & ZFCP_STATUS_FSFREQ_ERROR)) {
		set_host_byte(scpnt, DID_TRANSPORT_DISRUPTED);
		goto skip_fsfstatus;
	}

	switch (req->qtcb->header.fsf_status) {
	case FSF_INCONSISTENT_PROT_DATA:
	case FSF_INVALID_PROT_PARM:
		set_host_byte(scpnt, DID_ERROR);
		goto skip_fsfstatus;
	case FSF_BLOCK_GUARD_CHECK_FAILURE:
		zfcp_scsi_dif_sense_error(scpnt, 0x1);
		goto skip_fsfstatus;
	case FSF_APP_TAG_CHECK_FAILURE:
		zfcp_scsi_dif_sense_error(scpnt, 0x2);
		goto skip_fsfstatus;
	case FSF_REF_TAG_CHECK_FAILURE:
		zfcp_scsi_dif_sense_error(scpnt, 0x3);
		goto skip_fsfstatus;
	}
	fcp_rsp = (struct fcp_resp_with_ext *) &req->qtcb->bottom.io.fcp_rsp;
	zfcp_fc_eval_fcp_rsp(fcp_rsp, scpnt);

	if (req->adapter->adapter_features & FSF_FEATURE_MEASUREMENT_DATA)
		zfcp_fsf_req_latency(req);

	zfcp_fsf_trace_latency(req);

skip_fsfstatus:
	if (scpnt->result != 0)
		zfcp_dbf_scsi_result("erro", 3, req->adapter->dbf, scpnt, req);
	else if (scpnt->retries > 0)
		zfcp_dbf_scsi_result("retr", 4, req->adapter->dbf, scpnt, req);
	else
		zfcp_dbf_scsi_result("norm", 6, req->adapter->dbf, scpnt, req);

	scpnt->host_scribble = NULL;
	(scpnt->scsi_done) (scpnt);
	/*
	 * We must hold this lock until scsi_done has been called.
	 * Otherwise we may call scsi_done after abort regarding this
	 * command has completed.
	 * Note: scsi_done must not block!
	 */
	read_unlock_irqrestore(&req->adapter->abort_lock, flags);
}

static void zfcp_fsf_send_fcp_ctm_handler(struct zfcp_fsf_req *req)
{
	struct fcp_resp_with_ext *fcp_rsp;
	struct fcp_resp_rsp_info *rsp_info;

	fcp_rsp = (struct fcp_resp_with_ext *) &req->qtcb->bottom.io.fcp_rsp;
	rsp_info = (struct fcp_resp_rsp_info *) &fcp_rsp[1];

	if ((rsp_info->rsp_code != FCP_TMF_CMPL) ||
	     (req->status & ZFCP_STATUS_FSFREQ_ERROR))
		req->status |= ZFCP_STATUS_FSFREQ_TMFUNCFAILED;
}


static void zfcp_fsf_send_fcp_command_handler(struct zfcp_fsf_req *req)
{
	struct zfcp_unit *unit;
	struct fsf_qtcb_header *header = &req->qtcb->header;

	if (unlikely(req->status & ZFCP_STATUS_FSFREQ_TASK_MANAGEMENT))
		unit = req->data;
	else
		unit = req->unit;

	if (unlikely(req->status & ZFCP_STATUS_FSFREQ_ERROR))
		goto skip_fsfstatus;

	switch (header->fsf_status) {
	case FSF_HANDLE_MISMATCH:
	case FSF_PORT_HANDLE_NOT_VALID:
		zfcp_erp_adapter_reopen(unit->port->adapter, 0, "fssfch1", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_FCPLUN_NOT_VALID:
	case FSF_LUN_HANDLE_NOT_VALID:
		zfcp_erp_port_reopen(unit->port, 0, "fssfch2", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_SERVICE_CLASS_NOT_SUPPORTED:
		zfcp_fsf_class_not_supp(req);
		break;
	case FSF_ACCESS_DENIED:
		zfcp_fsf_access_denied_unit(req, unit);
		break;
	case FSF_DIRECTION_INDICATOR_NOT_VALID:
		dev_err(&req->adapter->ccw_device->dev,
			"Incorrect direction %d, unit 0x%016Lx on port "
			"0x%016Lx closed\n",
			req->qtcb->bottom.io.data_direction,
			(unsigned long long)unit->fcp_lun,
			(unsigned long long)unit->port->wwpn);
		zfcp_erp_adapter_shutdown(unit->port->adapter, 0, "fssfch3",
					  req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_CMND_LENGTH_NOT_VALID:
		dev_err(&req->adapter->ccw_device->dev,
			"Incorrect CDB length %d, unit 0x%016Lx on "
			"port 0x%016Lx closed\n",
			req->qtcb->bottom.io.fcp_cmnd_length,
			(unsigned long long)unit->fcp_lun,
			(unsigned long long)unit->port->wwpn);
		zfcp_erp_adapter_shutdown(unit->port->adapter, 0, "fssfch4",
					  req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_PORT_BOXED:
		zfcp_erp_port_boxed(unit->port, "fssfch5", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_LUN_BOXED:
		zfcp_erp_unit_boxed(unit, "fssfch6", req);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	case FSF_ADAPTER_STATUS_AVAILABLE:
		if (header->fsf_status_qual.word[0] ==
		    FSF_SQ_INVOKE_LINK_TEST_PROCEDURE)
			zfcp_fc_test_link(unit->port);
		req->status |= ZFCP_STATUS_FSFREQ_ERROR;
		break;
	}
skip_fsfstatus:
	if (req->status & ZFCP_STATUS_FSFREQ_TASK_MANAGEMENT)
		zfcp_fsf_send_fcp_ctm_handler(req);
	else {
		zfcp_fsf_send_fcp_command_task_handler(req);
		req->unit = NULL;
		zfcp_unit_put(unit);
	}
}

static int zfcp_fsf_set_data_dir(struct scsi_cmnd *scsi_cmnd, u32 *data_dir)
{
	switch (scsi_get_prot_op(scsi_cmnd)) {
	case SCSI_PROT_NORMAL:
		switch (scsi_cmnd->sc_data_direction) {
		case DMA_NONE:
			*data_dir = FSF_DATADIR_CMND;
			break;
		case DMA_FROM_DEVICE:
			*data_dir = FSF_DATADIR_READ;
			break;
		case DMA_TO_DEVICE:
			*data_dir = FSF_DATADIR_WRITE;
			break;
		case DMA_BIDIRECTIONAL:
			return -EINVAL;
		}
		break;

	case SCSI_PROT_READ_STRIP:
		*data_dir = FSF_DATADIR_DIF_READ_STRIP;
		break;
	case SCSI_PROT_WRITE_INSERT:
		*data_dir = FSF_DATADIR_DIF_WRITE_INSERT;
		break;
	case SCSI_PROT_READ_PASS:
		*data_dir = FSF_DATADIR_DIF_READ_CONVERT;
		break;
	case SCSI_PROT_WRITE_PASS:
		*data_dir = FSF_DATADIR_DIF_WRITE_CONVERT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * zfcp_fsf_send_fcp_command_task - initiate an FCP command (for a SCSI command)
 * @unit: unit where command is sent to
 * @scsi_cmnd: scsi command to be sent
 */
int zfcp_fsf_send_fcp_command_task(struct zfcp_unit *unit,
				   struct scsi_cmnd *scsi_cmnd)
{
	struct zfcp_fsf_req *req;
	struct fcp_cmnd *fcp_cmnd;
	u8 sbtype = SBAL_SFLAGS0_TYPE_READ;
	int retval = -EIO;
	struct zfcp_adapter *adapter = unit->port->adapter;
	struct zfcp_qdio *qdio = adapter->qdio;
	struct fsf_qtcb_bottom_io *io;

	if (unlikely(!(atomic_read(&unit->status) &
		       ZFCP_STATUS_COMMON_UNBLOCKED)))
		return -EBUSY;

	spin_lock(&qdio->req_q_lock);
	if (atomic_read(&qdio->req_q.count) <= 0) {
		atomic_inc(&qdio->req_q_full);
		goto out;
	}

	if (scsi_cmnd->sc_data_direction == DMA_TO_DEVICE)
		sbtype = SBAL_SFLAGS0_TYPE_WRITE;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_FCP_CMND,
				  adapter->pool.scsi_req);

	if (IS_ERR(req)) {
		retval = PTR_ERR(req);
		goto out;
	}

	scsi_cmnd->host_scribble = (unsigned char *) req->req_id;

	io = &req->qtcb->bottom.io;
	req->status |= ZFCP_STATUS_FSFREQ_CLEANUP;
	zfcp_unit_get(unit);
	req->unit = unit;
	req->data = scsi_cmnd;
	req->handler = zfcp_fsf_send_fcp_command_handler;
	req->qtcb->header.lun_handle = unit->handle;
	req->qtcb->header.port_handle = unit->port->handle;
	io->service_class = FSF_CLASS_3;
	io->fcp_cmnd_length = FCP_CMND_LEN;

	if (scsi_get_prot_op(scsi_cmnd) != SCSI_PROT_NORMAL) {
		io->data_block_length = scsi_cmnd->device->sector_size;
		io->ref_tag_value = scsi_get_lba(scsi_cmnd) & 0xFFFFFFFF;
	}

	if (zfcp_fsf_set_data_dir(scsi_cmnd, &io->data_direction))
		goto failed_scsi_cmnd;

	fcp_cmnd = (struct fcp_cmnd *) &req->qtcb->bottom.io.fcp_cmnd;
	zfcp_fc_scsi_to_fcp(fcp_cmnd, scsi_cmnd);

	if (scsi_prot_sg_count(scsi_cmnd)) {
		zfcp_qdio_set_data_div(qdio, &req->queue_req,
				       scsi_prot_sg_count(scsi_cmnd));
		retval = zfcp_qdio_sbals_from_sg(qdio, &req->queue_req,
						 sbtype,
						 scsi_prot_sglist(scsi_cmnd),
						 FSF_MAX_SBALS_PER_REQ);
		if (retval)
			goto failed_scsi_cmnd;
		io->prot_data_length = zfcp_qdio_real_bytes(
						scsi_prot_sglist(scsi_cmnd));
	}

	retval = zfcp_qdio_sbals_from_sg(qdio, &req->queue_req, sbtype,
					 scsi_sglist(scsi_cmnd),
					 FSF_MAX_SBALS_PER_REQ);
	if (unlikely(retval))
		goto failed_scsi_cmnd;

	zfcp_qdio_set_sbale_last(adapter->qdio, &req->queue_req);
	if (zfcp_adapter_multi_buffer_active(adapter))
		zfcp_qdio_set_scount(qdio, &req->queue_req);

	retval = zfcp_fsf_req_send(req);
	if (unlikely(retval))
		goto failed_scsi_cmnd;

	goto out;

failed_scsi_cmnd:
	zfcp_unit_put(unit);
	zfcp_fsf_req_free(req);
	scsi_cmnd->host_scribble = NULL;
out:
	spin_unlock(&qdio->req_q_lock);
	return retval;
}

/**
 * zfcp_fsf_send_fcp_ctm - send SCSI task management command
 * @unit: pointer to struct zfcp_unit
 * @tm_flags: unsigned byte for task management flags
 * Returns: on success pointer to struct fsf_req, NULL otherwise
 */
struct zfcp_fsf_req *zfcp_fsf_send_fcp_ctm(struct zfcp_unit *unit, u8 tm_flags)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *req = NULL;
	struct fcp_cmnd *fcp_cmnd;
	struct zfcp_qdio *qdio = unit->port->adapter->qdio;

	if (unlikely(!(atomic_read(&unit->status) &
		       ZFCP_STATUS_COMMON_UNBLOCKED)))
		return NULL;

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, FSF_QTCB_FCP_CMND,
				  qdio->adapter->pool.scsi_req);

	if (IS_ERR(req)) {
		req = NULL;
		goto out;
	}

	req->status |= ZFCP_STATUS_FSFREQ_TASK_MANAGEMENT;
	req->data = unit;
	req->handler = zfcp_fsf_send_fcp_command_handler;
	req->qtcb->header.lun_handle = unit->handle;
	req->qtcb->header.port_handle = unit->port->handle;
	req->qtcb->bottom.io.data_direction = FSF_DATADIR_CMND;
	req->qtcb->bottom.io.service_class = FSF_CLASS_3;
	req->qtcb->bottom.io.fcp_cmnd_length = FCP_CMND_LEN;

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= SBAL_SFLAGS0_TYPE_WRITE;
	sbale[1].eflags |= SBAL_EFLAGS_LAST_ENTRY;

	fcp_cmnd = (struct fcp_cmnd *) &req->qtcb->bottom.io.fcp_cmnd;
	zfcp_fc_fcp_tm(fcp_cmnd, unit->device, tm_flags);

	zfcp_fsf_start_timer(req, ZFCP_SCSI_ER_TIMEOUT);
	if (!zfcp_fsf_req_send(req))
		goto out;

	zfcp_fsf_req_free(req);
	req = NULL;
out:
	spin_unlock_bh(&qdio->req_q_lock);
	return req;
}

static void zfcp_fsf_control_file_handler(struct zfcp_fsf_req *req)
{
}

/**
 * zfcp_fsf_control_file - control file upload/download
 * @adapter: pointer to struct zfcp_adapter
 * @fsf_cfdc: pointer to struct zfcp_fsf_cfdc
 * Returns: on success pointer to struct zfcp_fsf_req, NULL otherwise
 */
struct zfcp_fsf_req *zfcp_fsf_control_file(struct zfcp_adapter *adapter,
					   struct zfcp_fsf_cfdc *fsf_cfdc)
{
	struct qdio_buffer_element *sbale;
	struct zfcp_qdio *qdio = adapter->qdio;
	struct zfcp_fsf_req *req = NULL;
	struct fsf_qtcb_bottom_support *bottom;
	int retval = -EIO;
	u8 direction;

	if (!(adapter->adapter_features & FSF_FEATURE_CFDC))
		return ERR_PTR(-EOPNOTSUPP);

	switch (fsf_cfdc->command) {
	case FSF_QTCB_DOWNLOAD_CONTROL_FILE:
		direction = SBAL_SFLAGS0_TYPE_WRITE;
		break;
	case FSF_QTCB_UPLOAD_CONTROL_FILE:
		direction = SBAL_SFLAGS0_TYPE_READ;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	spin_lock_bh(&qdio->req_q_lock);
	if (zfcp_fsf_req_sbal_get(qdio))
		goto out;

	req = zfcp_fsf_req_create(qdio, fsf_cfdc->command, NULL);
	if (IS_ERR(req)) {
		retval = -EPERM;
		goto out;
	}

	req->handler = zfcp_fsf_control_file_handler;

	sbale = zfcp_qdio_sbale_req(qdio, &req->queue_req);
	sbale[0].sflags |= direction;

	bottom = &req->qtcb->bottom.support;
	bottom->operation_subtype = FSF_CFDC_OPERATION_SUBTYPE;
	bottom->option = fsf_cfdc->option;

	retval = zfcp_qdio_sbals_from_sg(qdio, &req->queue_req,
					 direction, fsf_cfdc->sg,
					 FSF_MAX_SBALS_PER_REQ);
	if (retval ||
	    (zfcp_qdio_real_bytes(fsf_cfdc->sg) != ZFCP_CFDC_MAX_SIZE)) {
		zfcp_fsf_req_free(req);
		retval = -EIO;
		goto out;
	}
	zfcp_qdio_set_sbale_last(qdio, &req->queue_req);
	if (zfcp_adapter_multi_buffer_active(adapter))
		zfcp_qdio_set_scount(qdio, &req->queue_req);

	zfcp_fsf_start_timer(req, ZFCP_FSF_REQUEST_TIMEOUT);
	retval = zfcp_fsf_req_send(req);
out:
	spin_unlock_bh(&qdio->req_q_lock);

	if (!retval) {
		wait_for_completion(&req->completion);
		return req;
	}
	return ERR_PTR(retval);
}

/**
 * zfcp_fsf_reqid_check - validate req_id contained in SBAL returned by QDIO
 * @adapter: pointer to struct zfcp_adapter
 * @sbal_idx: response queue index of SBAL to be processed
 */
void zfcp_fsf_reqid_check(struct zfcp_qdio *qdio, int sbal_idx)
{
	struct zfcp_adapter *adapter = qdio->adapter;
	struct qdio_buffer *sbal = qdio->resp_q.sbal[sbal_idx];
	struct qdio_buffer_element *sbale;
	struct zfcp_fsf_req *fsf_req;
	unsigned long flags, req_id;
	int idx;

	for (idx = 0; idx < QDIO_MAX_ELEMENTS_PER_BUFFER; idx++) {

		sbale = &sbal->element[idx];
		req_id = (unsigned long) sbale->addr;
		spin_lock_irqsave(&adapter->req_list_lock, flags);
		fsf_req = zfcp_reqlist_find(adapter, req_id);

		if (!fsf_req) {
			/*
			 * Unknown request means that we have potentially memory
			 * corruption and must stop the machine immediately.
			 */
			zfcp_qdio_siosl(adapter);
			panic("error: unknown req_id (%lx) on adapter %s.\n",
			      req_id, dev_name(&adapter->ccw_device->dev));
		}

		list_del(&fsf_req->list);
		spin_unlock_irqrestore(&adapter->req_list_lock, flags);

		fsf_req->queue_req.sbal_response = sbal_idx;
		fsf_req->queue_req.qdio_inb_usage =
			atomic_read(&qdio->resp_q.count);
		zfcp_fsf_req_complete(fsf_req);

		if (likely(sbale->eflags & SBAL_EFLAGS_LAST_ENTRY))
			break;
	}
}
