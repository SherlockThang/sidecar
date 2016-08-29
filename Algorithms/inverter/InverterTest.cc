#include "ace/FILE_Connector.h"
#include "ace/Reactor.h"

#include "Algorithms/Controller.h"
#include "Algorithms/ShutdownMonitor.h"
#include "IO/Readers.h"
#include "IO/FileWriterTask.h"
#include "IO/MessageManager.h"
#include "IO/ProcessingStateChangeRequest.h"
#include "IO/ShutdownRequest.h"
#include "IO/Stream.h"

#include "Logger/Log.h"
#include "Messages/Video.h"
#include "Parameter/Parameter.h"
#include "UnitTest/UnitTest.h"
#include "Utils/FilePath.h"

#include "Inverter.h"

using namespace SideCar::Algorithms;
using namespace SideCar::IO;
using namespace SideCar::Messages;

struct Test : public UnitTest::TestObj
{
    Test() : UnitTest::TestObj("Inverter") {}
    void test();
};

void
Test::test()
{
    Logger::Log::Root().setPriorityLimit(Logger::Priority::kDebug);
    Utils::TemporaryFilePath testOutputPath("inverterTestOutput");
    {
	Stream::Ref stream(Stream::Make("test"));

	assertEqual(0, stream->push(new ShutdownMonitorModule(stream)));

        FileWriterTaskModule* writer = new FileWriterTaskModule(stream);
	assertEqual(0, stream->push(writer));
	assertTrue(writer->getTask()->openAndInit("Video", testOutputPath.filePath()));

	ControllerModule* controller = new ControllerModule(stream);
	assertEqual(0, stream->push(controller));
	assertTrue(controller->getTask()->openAndInit("Inverter"));

	stream->put(ProcessingStateChangeRequest(ProcessingState::kRun).getWrapped());

	Inverter* inverter = dynamic_cast<Inverter*>(controller->getTask()->getAlgorithm());
	assertTrue(inverter);
	inverter->setMin(1);
	inverter->setMax(10);

	VMEDataMessage vme;
	vme.header.azimuth = 0;
	int16_t init[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	Video::Ref msg(Video::Make("test", vme, init, init + 10));
	MessageManager mgr(msg);
	stream->put(mgr.getMessage(), 0);
	assertFalse(mgr.hasEncoded());

	stream->put(ShutdownRequest().getWrapped());
	ACE_Reactor::instance()->run_reactor_event_loop();
	writer->getTask()->close(1);
    }

    FileReader::Ref reader(new FileReader);
    ACE_FILE_Addr inputAddr(testOutputPath.filePath().c_str());
    ACE_FILE_Connector inputConnector(reader->getDevice(), inputAddr);
    assertTrue(reader->fetchInput());
    assertTrue(reader->isMessageAvailable());
    Decoder decoder(reader->getMessage());
    {
	Video::Ref msg(decoder.decode<Video>());
	assertEqual(size_t(10), msg->size());
	Video::const_iterator pos = msg->begin();
	assertEqual(10, *pos++);
	assertEqual(9, *pos++);
	assertEqual(8, *pos++);
	assertEqual(7, *pos++);
	assertEqual(6, *pos++);
	assertEqual(5, *pos++);
	assertEqual(4, *pos++);
	assertEqual(3, *pos++);
	assertEqual(2, *pos++);
	assertEqual(1, *pos++);
	assertTrue(pos == msg->end());
    }
    
    // Uncomment the following to fail the test and see the log results. assertTrue(false);
}

int
main(int argc, const char* argv[])
{
    return Test().mainRun();
}
