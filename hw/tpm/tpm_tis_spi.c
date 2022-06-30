#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/acpi/tpm.h"
#include "tpm_prop.h"
#include "tpm_tis.h"
#include "qom/object.h"
#include "hw/ssi/ssi.h"

typedef enum {
    STATE_IDLE,
    STATE_ADDRESS,
    STATE_DATA,
} CMDState;

union TpmTisRWSizeByte {
    uint8_t byte;
    struct {
        uint8_t data__expected_size: 6;
        uint8_t resv: 1;
        uint8_t rwflag: 1;
    }
}

union TpmTisSpiHwAddr {
    hwaddr addr;
    uint8_t bytes[sizeof(hwaddr)];
}

union TpmTisSpiData {
    uint32_t data;
    uint8_t bytes[sizeof(data)];
}

struct TPMStateSpi {
    /*< private >*/
    SSIPeripheral parent_obj;

    /*< public >*/
    TPMState state; /* not a QOM object */
    uint8_t spi_state;

    union TpmTisRWSizeByte first_byte;
    union TpmTisSpiHwAddr addr;
    union TpmTisSpiData data;

    uint8_t data_idx;
    uint8_t addr_idx;
};

OBJECT_DECLARE_SIMPLE_TYPE(TPMStateSpi, TPM_TIS_SPI)

static uint32_t tpm_tis_spi_transfer8(SSIPeripheral *ss, uint32_t tx)
{
    TPMStateSpi *tts = TPM_TIS_SPI(ss);
    uint32_t r = 0;

    switch (s->state) {
        case STATE_ADDRESS:
            if (tts->addr_idx == 0) {
                tts->state = STATE_DATA;
                break;
            }
            tts->addr.bytes[--tts->addr_idx] = (uint8_t)tx;
            break;
        case STATE_DATA:
           if (tts->data_idx == 0) {
               if (tts->first_byte.rwflag) {
                   r = tpm_tis_mmio_read(tts, tts->addr.addr,
                                         tts->first_byte.data_expected_size);
               } else {
                   tpm_tis_mmio_write(tts, tts->addr.addr, tts->data.data,
                                      tts->first_byte.data_expected_size);
               }
               tts->state = STATE_IDLE;
               break;
           }
           tts->data.bytes[--tts->data_idx] = (uint8_t)tx;
           break;
        case STATE_IDLE:
            tts->first_byte.byte = (uint8_t)tx;
            tts->data_idx = tts->first_byte.data_expected_size;
            tts->state = STATE_ADDRESS;
            break;
        default:
            tts->first_byte.byte = (uint8_t)tx;
            tts->data_idx = tts->first_byte.data_expected_size;
            tts->state = STATE_ADDRESS;
            break;
    }
    return r;
}

// static int tpm_tis_spi_cs(SSIPeripheral *ss, bool select)
// {
//     TPMStateSpi *tts = TPM_TIS_SPI(ss);

//     if (select) {
//     }

//     return 0;
// }

static int tpm_tis_pre_save_spi(void *opaque)
{
    TPMStateSpi *sbdev = opaque;

    return tpm_tis_pre_save(&sbdev->state);
}

static const VMStateDescription vmstate_tpm_tis_spi = {
    .name = "tpm-tis-spi",
    .version_id = 0,
    .pre_save  = tpm_tis_pre_save_spi,
    .fields = (VMStateField[]) {
        VMSTATE_BUFFER(state.buffer, TPMStateSpi),
        VMSTATE_UINT16(state.rw_offset, TPMStateSpi),
        VMSTATE_UINT8(state.active_locty, TPMStateSpi),
        VMSTATE_UINT8(state.aborting_locty, TPMStateSpi),
        VMSTATE_UINT8(state.next_locty, TPMStateSpi),

        VMSTATE_STRUCT_ARRAY(state.loc, TPMStateSpi, TPM_TIS_NUM_LOCALITIES,
                             0, vmstate_locty, TPMLocality),

        VMSTATE_END_OF_LIST()
    }
};

static void tpm_tis_spi_request_completed(TPMIf *ti, int ret)
{
    TPMStateSpi *sbdev = TPM_TIS_SPI(ti);
    TPMState *s = &sbdev->state;

    tpm_tis_request_completed(s, ret);
}

static enum TPMVersion tpm_tis_spi_get_tpm_version(TPMIf *ti)
{
    TPMStateSpi *sbdev = TPM_TIS_SPI(ti);
    TPMState *s = &sbdev->state;

    return tpm_tis_get_tpm_version(s);
}

static void tpm_tis_spi_reset(DeviceState *dev)
{
    TPMStateSpi *sbdev = TPM_TIS_SPI(dev);
    TPMState *s = &sbdev->state;

    return tpm_tis_reset(s);
}

static Property tpm_tis_spi_properties[] = {
    DEFINE_PROP_UINT32("irq", TPMStateSpi, state.irq_num, TPM_TIS_IRQ),
    DEFINE_PROP_TPMBE("tpmdev", TPMStateSpi, state.be_driver),
    DEFINE_PROP_BOOL("ppi", TPMStateSpi, state.ppi_enabled, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void tpm_tis_spi_initfn(Object *obj)
{
    TPMStateSpi *sbdev = TPM_TIS_SPI(obj);
    TPMState *s = &sbdev->state;

    sbdev->addr_idx = 3;
}

static void tpm_tis_spi_realizefn(DeviceState *dev, Error **errp)
{
    TPMStateSpi *sbdev = TPM_TIS_SPI(dev);
    TPMState *s = &sbdev->state;

    if (!tpm_find()) {
        error_setg(errp, "at most one TPM device is permitted");
        return;
    }

    if (!s->be_driver) {
        error_setg(errp, "'tpmdev' property is required");
        return;
    }
}

static void tpm_tis_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    TPMIfClass *tc = TPM_IF_CLASS(klass);

    device_class_set_props(dc, tpm_tis_spi_properties);

    k->transfer = tpm_tis_spi_transfer8;
    // k->set_cs = tpm_tis_spi_cs;
    dc->vmsd  = &vmstate_tpm_tis_spi;
    tc->model = TPM_MODEL_TPM_TIS;
    dc->realize = tpm_tis_spi_realizefn;
    dc->reset = tpm_tis_spi_reset;
    tc->request_completed = tpm_tis_spi_request_completed;
    tc->get_version = tpm_tis_spi_get_tpm_version;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo tpm_tis_spi_info = {
    .name = TYPE_TPM_TIS_SPI,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(TPMStateSpi),
    .instance_init = tpm_tis_spi_initfn,
    .class_init  = tpm_tis_spi_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_TPM_IF },
        { }
    }
};

static void tpm_tis_spi_register(void)
{
    type_register_static(&tpm_tis_spi_info);
}

type_init(tpm_tis_spi_register)
