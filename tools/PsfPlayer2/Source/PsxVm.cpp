#include <stdexcept>
#include <assert.h>
#include "PsxVm.h"
#include "Log.h"
#include "MA_MIPSIV.h"

#define LOG_NAME ("psxvm")

using namespace std;
using namespace std::tr1;
using namespace boost;
using namespace Psx;

CPsxVm::CPsxVm() :
m_cpu(MEMORYMAP_ENDIAN_LSBF, 0, 0x1FFFFFFF),
m_executor(m_cpu),
m_bios(m_cpu),
m_status(PAUSED),
m_singleStep(false),
m_ram(new uint8[RAMSIZE]),
m_dmac(m_ram, m_intc),
m_thread(bind(&CPsxVm::ThreadProc, this))
{
	//Read memory map
	m_cpu.m_pMemoryMap->InsertReadMap(0,			RAMSIZE - 1,	m_ram,										0x01);
	m_cpu.m_pMemoryMap->InsertReadMap(HW_REG_BEGIN,	HW_REG_END,		bind(&CPsxVm::ReadIoRegister, this, _1),	0x02);

	//Write memory map
	m_cpu.m_pMemoryMap->InsertWriteMap(0,				RAMSIZE - 1,	m_ram,											0x01);
	m_cpu.m_pMemoryMap->InsertWriteMap(HW_REG_BEGIN,	HW_REG_END,		bind(&CPsxVm::WriteIoRegister, this, _1, _2),	0x02);

	m_cpu.m_pArch = &g_MAMIPSIV;
	m_cpu.m_pAddrTranslator = &CMIPS::TranslateAddress64;

	m_cpu.m_Functions.Unserialize("rawr.functions");
	m_cpu.m_Comments.Unserialize("rawr.comments");

	m_dmac.SetReceiveFunction(4, bind(&CSpu::ReceiveDma, &m_spu, _1, _2, _3));
}

CPsxVm::~CPsxVm()
{
	m_cpu.m_Functions.Serialize("rawr.functions");
	m_cpu.m_Comments.Serialize("rawr.comments");
	delete [] m_ram;
}

void CPsxVm::Reset()
{
	memset(m_ram, 0, RAMSIZE);
	m_cpu.Reset();
	m_bios.Reset();
	m_spu.Reset();
}

uint32 CPsxVm::ReadIoRegister(uint32 address)
{
	if(address >= CSpu::SPU_BEGIN && address <= CSpu::SPU_END)
	{
		return m_spu.ReadRegister(address);
	}
	else if(address >= CDmac::ADDR_BEGIN && address <= CDmac::ADDR_END)
	{
		return m_dmac.ReadRegister(address);
	}
	else if(address >= CIntc::ADDR_BEGIN && address <= CIntc::ADDR_END)
	{
		return m_intc.ReadRegister(address);
	}
	else
	{
		CLog::GetInstance().Print(LOG_NAME, "Reading an unknown hardware register (0x%0.8X).\r\n", address);
	}
	return 0;
}

uint32 CPsxVm::WriteIoRegister(uint32 address, uint32 value)
{
	if(address >= CSpu::SPU_BEGIN && address <= CSpu::SPU_END)
	{
		m_spu.WriteRegister(address, static_cast<uint16>(value));
	}
	else if(address >= CDmac::ADDR_BEGIN && address <= CDmac::ADDR_END)
	{
		m_dmac.WriteRegister(address, value);
	}
	else if(address >= CIntc::ADDR_BEGIN && address <= CIntc::ADDR_END)
	{
		m_intc.WriteRegister(address, value);
	}
	else
	{
		CLog::GetInstance().Print(LOG_NAME, "Writing to an unknown hardware register (0x%0.8X, 0x%0.8X).\r\n", address, value);
	}
	return 0;
}

void CPsxVm::LoadExe(uint8* exe)
{
	EXEHEADER* exeHeader(reinterpret_cast<EXEHEADER*>(exe));
	if(strncmp(reinterpret_cast<char*>(exeHeader->id), "PS-X EXE", 8))
	{
		throw runtime_error("Invalid PSX executable.");
	}

	m_cpu.m_State.nPC					= exeHeader->pc0 & 0x1FFFFFFF;
	m_cpu.m_State.nGPR[CMIPS::GP].nD0	= exeHeader->gp0;
	m_cpu.m_State.nGPR[CMIPS::SP].nD0	= exeHeader->stackAddr;

	exe += 0x800;
	if(exeHeader->textAddr != 0)
	{
		uint32 realAddr = exeHeader->textAddr & 0x1FFFFFFF;
		assert(realAddr + exeHeader->textSize <= RAMSIZE);
		memcpy(m_ram + realAddr, exe, exeHeader->textSize);
		exe += exeHeader->textSize;
	}
}

CVirtualMachine::STATUS CPsxVm::GetStatus() const
{
	return m_status;
}

void CPsxVm::Pause()
{
	m_status = PAUSED;
	m_OnRunningStateChange();
	m_OnMachineStateChange();
}

void CPsxVm::Resume()
{
	m_status = RUNNING;
	m_OnRunningStateChange();
}

CMIPS& CPsxVm::GetCpu()
{
	return m_cpu;
}

void CPsxVm::Step()
{
	m_singleStep = true;
	m_status = RUNNING;
	m_OnRunningStateChange();
}

void CPsxVm::ExecuteCpu(bool singleStep)
{
    if(!m_cpu.m_State.nHasException)
    {
		if(m_intc.IsInterruptPending())
		{
			m_bios.HandleInterrupt();
        }
    }
	if(!m_cpu.m_State.nHasException)
	{
		m_executor.Execute(singleStep ? 1 : 5000);
	}
	if(m_cpu.m_State.nHasException)
	{
		m_bios.HandleException();
	}
}

void CPsxVm::ThreadProc()
{
	while(1)
	{
		if(m_status == PAUSED)
		{
            //Sleep during 100ms
            xtime xt;
            xtime_get(&xt, boost::TIME_UTC);
            xt.nsec += 100 * 1000000;
			thread::sleep(xt);
		}
		else
		{
			ExecuteCpu(m_singleStep);
			if(m_executor.MustBreak() || m_singleStep)
			{
				m_status = PAUSED;
				m_singleStep = false;
				m_OnMachineStateChange();
				m_OnRunningStateChange();
			}
		}
	}
}
