/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file StateTests.cpp
 * @author Dimitry Khokhlov <dimitry@ethereum.org>
 * @date 2016
 * General State Tests parser.
 */

#include <boost/filesystem/operations.hpp>
#include <boost/test/unit_test.hpp>
#include <thread>
#include <mutex>

#include <dataObject/DataObject.h>
#include <libdevcore/CommonData.h>
#include <libdevcore/CommonIO.h>
#include <retesteth/ExitHandler.h>
#include <retesteth/Options.h>
#include <retesteth/RPCSession.h>
#include <retesteth/TestHelper.h>
#include <retesteth/TestOutputHelper.h>
#include <retesteth/TestSuite.h>
#include <retesteth/ethObjects/common.h>
#include <retesteth/testSuites/Common.h>
#include <retesteth/testSuites/StateTests.h>
#include <retesteth/testSuites/blockchain/BlockchainTests.h>

using namespace std;
using namespace dev;
namespace fs = boost::filesystem;

namespace
{
bool OptionsAllowTransaction(scheme_generalTransaction::transactionInfo const& _tr)
{
    Options const& opt = Options::get();
    if ((opt.trDataIndex == (int)_tr.dataInd || opt.trDataIndex == -1) &&
        (opt.trGasIndex == (int)_tr.gasInd || opt.trGasIndex == -1) &&
        (opt.trValueIndex == (int)_tr.valueInd || opt.trValueIndex == -1))
        return true;
    return false;
}

/// Generate a blockchain test from state test filler
DataObject FillTestAsBlockchain(DataObject const& _testFile)
{
    DataObject filledTest;
    test::scheme_stateTestFiller test(_testFile);

    RPCSession& session = RPCSession::instance(TestOutputHelper::getThreadID());
    // run transactions on all networks that we need
    for (auto const& net : test.getExpectSection().getAllNetworksFromExpectSection())
    {
        // run transactions for defined expect sections only
        for (auto const& expect : test.getExpectSection().getExpectSections())
        {
            // if expect section for this networks
            if (expect.getNetworks().count(net))
            {
                for (auto& tr : test.getTransactionsUnsafe())
                {
                    if (!OptionsAllowTransaction(tr))
                        continue;

                    // if expect section is for this transaction
                    if (!expect.checkIndexes(tr.dataInd, tr.gasInd, tr.valueInd))
                        continue;

                    TestOutputHelper::get().setCurrentTestInfo(
                        "Network: " + net + ", TrInfo: d: " + toString(tr.dataInd) +
                        ", g: " + toString(tr.gasInd) + ", v: " + toString(tr.valueInd) +
                        ", Test: " + TestOutputHelper::get().testName());

                    // State Tests does not have mining rewards
                    scheme_expectSectionElement mexpect = expect;
                    mexpect.correctMiningReward(net, test.getEnv().getCoinbase());

                    string sEngine = "NoProof";
                    session.test_setChainParams(test.getGenesisForRPC(net, sEngine).asJson());
                    u256 a(test.getEnv().getData().atKey("currentTimestamp").asString());
                    session.test_modifyTimestamp(a.convert_to<size_t>());
                    string signedTransactionRLP = tr.transaction.getSignedRLP();
                    string trHash = session.eth_sendRawTransaction(signedTransactionRLP);

                    if (!session.getLastRPCError().empty())
                        ETH_ERROR_MESSAGE(session.getLastRPCError());
                    if (!isHash<h256>(trHash))
                        ETH_ERROR_MESSAGE("eth_sendRawTransaction return invalid hash: '" + trHash +
                                          "' " + TestOutputHelper::get().testInfo());

                    string latestBlockNumber = session.test_mineBlocks(1);
                    tr.executed = true;

                    scheme_block remoteBlock =
                        session.eth_getBlockByNumber(latestBlockNumber, true);
                    scheme_state remoteState = getRemoteState(session, remoteBlock);
                    if (remoteState.isHash())
                        compareStates(mexpect.getExpectState(), session, remoteBlock);
                    else
                        compareStates(mexpect.getExpectState(), remoteState);

                    DataObject aBlockchainTest;
                    if (test.getData().count("_info"))
                        aBlockchainTest["_info"] = test.getData().atKey("_info");
                    aBlockchainTest["genesisBlockHeader"] = test.getEnv().getDataForRPC();
                    aBlockchainTest["pre"] = test.getPre().getData();
                    aBlockchainTest["postState"] = remoteState.getData();
                    aBlockchainTest["network"] = net;
                    aBlockchainTest["sealEngine"] = sEngine;
                    aBlockchainTest["lastblockhash"] = remoteBlock.getBlockHash();

                    test::scheme_block genesisBlock = session.eth_getBlockByNumber("0", true);
                    aBlockchainTest["genesisRLP"] = genesisBlock.getBlockRLP();

                    DataObject block;
                    block["rlp"] = remoteBlock.getBlockRLP();
                    block["blockHeader"] = remoteBlock.getBlockHeader();
                    aBlockchainTest["blocks"].addArrayObject(block);

                    string dataPostfix = "_d" + toString(tr.dataInd) + "g" + toString(tr.gasInd) +
                                         "v" + toString(tr.valueInd);
                    dataPostfix += "_" + net;

                    if (filledTest.count(_testFile.getKey() + dataPostfix))
                        ETH_ERROR_MESSAGE("The test filler contain redundunt expect section: " +
                                          TestOutputHelper::get().testInfo());

                    filledTest[_testFile.getKey() + dataPostfix] = aBlockchainTest;
                    session.test_rewindToBlock(0);
                }
            }
        }
        test.checkUnexecutedTransactions();
    }
    return filledTest;
}

/// Rewrite the test file. Fill General State Test
DataObject FillTest(DataObject const& _testFile)
{
    DataObject filledTest;
    filledTest.setAutosort(true);
    test::scheme_stateTestFiller test(_testFile);

    RPCSession& session = RPCSession::instance(TestOutputHelper::getThreadID());
    if (test.getData().count("_info"))
        filledTest["_info"] = test.getData().atKey("_info");
    filledTest["env"] = test.getEnv().getData();
    filledTest["pre"] = test.getPre().getData();
    filledTest["transaction"] = test.getGenTransaction().getData();

    // run transactions on all networks that we need
    for (auto const& net : test.getExpectSection().getAllNetworksFromExpectSection())
    {
        DataObject forkResults;
        forkResults.setKey(net);
        session.test_setChainParams(test.getGenesisForRPC(net, "NoReward").asJson());

        // run transactions for defined expect sections only
        for (auto const& expect : test.getExpectSection().getExpectSections())
        {
            // if expect section for this networks
            if (expect.getNetworks().count(net))
            {
                for (auto& tr : test.getTransactionsUnsafe())
                {
                    if (!OptionsAllowTransaction(tr))
                        continue;

                    // if expect section is for this transaction
                    if (!expect.checkIndexes(tr.dataInd, tr.gasInd, tr.valueInd))
                        continue;

                    TestOutputHelper::get().setCurrentTestInfo(
                        "Network: " + net + ", TrInfo: d: " + toString(tr.dataInd) +
                        ", g: " + toString(tr.gasInd) + ", v: " + toString(tr.valueInd) +
                        ", Test: " + TestOutputHelper::get().testName());

                    u256 a(test.getEnv().getData().atKey("currentTimestamp").asString());
                    session.test_modifyTimestamp(a.convert_to<size_t>());
                    string trHash = session.eth_sendRawTransaction(tr.transaction.getSignedRLP());
                    string latestBlockNumber = session.test_mineBlocks(1);
                    tr.executed = true;

                    scheme_block blockInfo = session.eth_getBlockByNumber(latestBlockNumber, false);
                    compareStates(expect.getExpectState(), session, blockInfo);

                    DataObject indexes;
                    DataObject transactionResults;
                    indexes["data"] = tr.dataInd;
                    indexes["gas"] = tr.gasInd;
                    indexes["value"] = tr.valueInd;

                    transactionResults["indexes"] = indexes;
                    transactionResults["hash"] = blockInfo.getStateHash();

                    // Fill up the loghash (optional)
                    string logHash = session.test_getLogHash(trHash);
                    if (!logHash.empty())
                        transactionResults["logs"] = logHash;

                    forkResults.addArrayObject(transactionResults);
                    session.test_rewindToBlock(0);
                }
            }
        }
        test.checkUnexecutedTransactions();
        filledTest["post"].addSubObject(forkResults);
    }
    return filledTest;
}

/// Read and execute the test file
void RunTest(DataObject const& _testFile)
{
    test::scheme_stateTest test(_testFile);
    RPCSession& session = RPCSession::instance(TestOutputHelper::getThreadID());

    // read post state results
    for (auto const& post: test.getPost().getResults())
    {
        string const& network = post.first;
        if (!Options::get().singleTestNet.empty() && Options::get().singleTestNet != network)
            continue;

        session.test_setChainParams(test.getGenesisForRPC(network, "NoReward").asJson());

        // read all results for a specific fork
        for (auto const& result: post.second)
        {
            // look for a transaction with this indexes and execute it on a client
            for (auto& tr: test.getTransactionsUnsafe())
            {
                if (!OptionsAllowTransaction(tr))
                    continue;

                bool blockMined = false;
                if (result.checkIndexes(tr.dataInd, tr.gasInd, tr.valueInd))
                {
                    string testInfo = TestOutputHelper::get().testName() + ", fork: " + network
                                    + ", TrInfo: d: " + toString(tr.dataInd) + ", g: " + toString(tr.gasInd)
                                    + ", v: " + toString(tr.valueInd);
                    TestOutputHelper::get().setCurrentTestInfo(testInfo);
                    u256 a(test.getEnv().getData().atKey("currentTimestamp").asString());
                    session.test_modifyTimestamp(a.convert_to<size_t>());
                    string trHash = session.eth_sendRawTransaction(tr.transaction.getSignedRLP());
                    string latestBlockNumber = session.test_mineBlocks(1);
                    tr.executed = true;
                    blockMined = true;

                    // Validate post state
                    string postHash = result.getData().atKey("hash").asString();
                    scheme_block remoteBlockInfo =
                        session.eth_getBlockByNumber(latestBlockNumber, false);
                    validatePostHash(session, postHash, remoteBlockInfo);

                    // Validate log hash
                    string postLogHash = result.getData().atKey("logs").asString();
                    string remoteLogHash = session.test_getLogHash(trHash);
                    if (!remoteLogHash.empty() && remoteLogHash != postLogHash)
                    {
                        ETH_ERROR_MESSAGE("Error at " + TestOutputHelper::get().testInfo() +
                                          ", logs hash mismatch: '" + remoteLogHash + "'" +
                                          ", expected: '" + postLogHash + "'");
                    }
                }
                if (blockMined)
                    session.test_rewindToBlock(0);
            }
        }

        test.checkUnexecutedTransactions();
    }
}
}  // namespace closed

namespace test
{
DataObject StateTestSuite::doTests(DataObject const& _input, TestSuiteOptions& _opt) const
{
    checkDataObject(_input);

    // Do not check only one test if RUNNING a blockchain test with (--fillhchain)
    if (!Options::get().fillchain || _opt.doFilling)
        checkOnlyOneTest(_input);

    DataObject filledTest;
    DataObject const& inputTest = _input.getSubObjects().at(0);
    string const testname = inputTest.getKey();
    if (!TestOutputHelper::get().checkTest(testname))
        return filledTest;

    if (_opt.doFilling)
    {
        checkTestNameIsEqualToFileName(_input);
        DataObject outputTest;
        if (Options::get().fillchain)
        {
            // Each transaction will produce many tests
            outputTest = FillTestAsBlockchain(inputTest);
            for (auto const& obj : outputTest.getSubObjects())
                filledTest.addSubObject(obj);
        }
        else
        {
            outputTest[testname] = FillTest(inputTest);
            filledTest = outputTest;
        }
    }
    else
    {
        if (Options::get().fillchain)
        {
            BlockchainTestValidSuite bcTestSuite;
            bcTestSuite.doTests(_input, _opt);
        }
        else
            RunTest(inputTest);
    }
    return filledTest;
}

TestSuite::TestPath StateTestSuite::suiteFolder() const
{
    if (Options::get().fillchain)
        return TestSuite::TestPath(fs::path("BlockchainTests") / "GeneralStateTestsRetesteth");
    return TestSuite::TestPath(fs::path("GeneralStateTests"));
}

TestSuite::FillerPath StateTestSuite::suiteFillerFolder() const
{
    return TestSuite::FillerPath(fs::path("src") / "GeneralStateTestsFiller");
}

}// Namespace Close

class GeneralTestFixture
{
public:
    GeneralTestFixture()
    {
        test::StateTestSuite suite;
        string casename = boost::unit_test::framework::current_test_case().p_name;
        boost::filesystem::path suiteFillerPath = suite.getFullPathFiller(casename).parent_path();

        static vector<string> const timeConsumingTestSuites{
            string{"stTimeConsuming"}, string{"stQuadraticComplexityTest"}};
        if (test::inArray(timeConsumingTestSuites, casename) && !test::Options::get().all)
        {
            if (!ExitHandler::receivedExitSignal())
                std::cout << "Skipping " << casename << " because --all option is not specified.\n";
            test::TestOutputHelper::get().markTestFolderAsFinished(suiteFillerPath, casename);
            return;
        }
        suite.runAllTestsInFolder(casename);
        test::TestOutputHelper::get().markTestFolderAsFinished(suiteFillerPath, casename);
    }
};

BOOST_FIXTURE_TEST_SUITE(GeneralStateTests, GeneralTestFixture)

//Frontier Tests
BOOST_AUTO_TEST_CASE(stCallCodes){}
BOOST_AUTO_TEST_CASE(stCallCreateCallCodeTest){}
BOOST_AUTO_TEST_CASE(stExample){}
BOOST_AUTO_TEST_CASE(stInitCodeTest){}
BOOST_AUTO_TEST_CASE(stLogTests){}
BOOST_AUTO_TEST_CASE(stMemoryTest){}
BOOST_AUTO_TEST_CASE(stPreCompiledContracts){}
BOOST_AUTO_TEST_CASE(stPreCompiledContracts2){}
BOOST_AUTO_TEST_CASE(stRandom){}
BOOST_AUTO_TEST_CASE(stRandom2){}
BOOST_AUTO_TEST_CASE(stRecursiveCreate){}
BOOST_AUTO_TEST_CASE(stRefundTest){}
BOOST_AUTO_TEST_CASE(stSolidityTest){}
BOOST_AUTO_TEST_CASE(stSpecialTest){}
BOOST_AUTO_TEST_CASE(stSystemOperationsTest){}
BOOST_AUTO_TEST_CASE(stTransactionTest){}
BOOST_AUTO_TEST_CASE(stTransitionTest){}
BOOST_AUTO_TEST_CASE(stWalletTest){}

//Homestead Tests
BOOST_AUTO_TEST_CASE(stCallDelegateCodesCallCodeHomestead){}
BOOST_AUTO_TEST_CASE(stCallDelegateCodesHomestead){}
BOOST_AUTO_TEST_CASE(stHomesteadSpecific){}
BOOST_AUTO_TEST_CASE(stDelegatecallTestHomestead){}

//EIP150 Tests
BOOST_AUTO_TEST_CASE(stChangedEIP150){}
BOOST_AUTO_TEST_CASE(stEIP150singleCodeGasPrices){}
BOOST_AUTO_TEST_CASE(stMemExpandingEIP150Calls){}
BOOST_AUTO_TEST_CASE(stEIP150Specific){}

//EIP158 Tests
BOOST_AUTO_TEST_CASE(stEIP158Specific){}
BOOST_AUTO_TEST_CASE(stNonZeroCallsTest){}
BOOST_AUTO_TEST_CASE(stZeroCallsTest){}
BOOST_AUTO_TEST_CASE(stZeroCallsRevert){}
BOOST_AUTO_TEST_CASE(stCodeSizeLimit){}
BOOST_AUTO_TEST_CASE(stCreateTest){}
BOOST_AUTO_TEST_CASE(stRevertTest){}

//Metropolis Tests
BOOST_AUTO_TEST_CASE(stStackTests){}
BOOST_AUTO_TEST_CASE(stStaticCall){}
BOOST_AUTO_TEST_CASE(stReturnDataTest){}
BOOST_AUTO_TEST_CASE(stZeroKnowledge){}
BOOST_AUTO_TEST_CASE(stZeroKnowledge2){}
BOOST_AUTO_TEST_CASE(stCodeCopyTest){}
BOOST_AUTO_TEST_CASE(stBugs){}

//Constantinople Tests
BOOST_AUTO_TEST_CASE(stShift){}
BOOST_AUTO_TEST_CASE(stCreate2){}
BOOST_AUTO_TEST_CASE(stExtCodeHash){}
BOOST_AUTO_TEST_CASE(stSStoreTest){}

//Stress Tests
BOOST_AUTO_TEST_CASE(stAttackTest){}
BOOST_AUTO_TEST_CASE(stMemoryStressTest){}
BOOST_AUTO_TEST_CASE(stQuadraticComplexityTest){}

//Invalid Opcode Tests
BOOST_AUTO_TEST_CASE(stBadOpcode){}

//New Tests
BOOST_AUTO_TEST_CASE(stArgsZeroOneBalance){}
BOOST_AUTO_TEST_CASE(stEWASMTests){}
BOOST_AUTO_TEST_CASE(stTimeConsuming) {}
BOOST_AUTO_TEST_SUITE_END()

