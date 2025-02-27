#pragma once
#include "../object.h"
#include "scheme_account.h"

#include <retesteth/TestHelper.h>
#include <libdevcore/RLP.h>
#include <libdevcore/SHA3.h>
#include <libdevcrypto/Common.h>
using namespace dev;

namespace test {
class scheme_transaction : public object
{
public:
    scheme_transaction(DataObject const& _transaction) : object(_transaction)
    {
        if (_transaction.count("secretKey") > 0)
        {
            test::requireJsonFields(_transaction, "transaction",
                {{"data", {DataType::String}}, {"gasLimit", {DataType::String}},
                    {"gasPrice", {DataType::String}}, {"nonce", {DataType::String}},
                    {"secretKey", {DataType::String}}, {"to", {DataType::String}},
                    {"value", {DataType::String}}});
        }
        else
        {
            test::requireJsonFields(_transaction, "transaction",
                {{"data", {DataType::String}}, {"gasLimit", {DataType::String}},
                    {"gasPrice", {DataType::String}}, {"nonce", {DataType::String}},
                    {"v", {DataType::String}}, {"r", {DataType::String}}, {"s", {DataType::String}},
                    {"to", {DataType::String}}, {"value", {DataType::String}}});
        }

        m_data["version"] = "0x01";
        if (!m_data.atKey("to").asString().empty())
            m_data["to"] = scheme_account::makeHexAddress(m_data.atKey("to").asString());
        // convert into rpc format
        m_data["gas"] = m_data["gasLimit"].asString();
        makeAllFieldsHex(m_data);
    }

    std::string getSignedRLP() const
    {
        u256 nonce = u256(m_data.atKey("nonce").asString());
        u256 gasPrice = u256(m_data.atKey("gasPrice").asString());
        u256 gasLimit = u256(m_data.atKey("gasLimit").asString());
        Address trTo = Address(m_data.atKey("to").asString());
        u256 value = u256(m_data.atKey("value").asString());
        bytes data = fromHex(m_data.atKey("data").asString());

        dev::RLPStream s;
        s.appendList(6);
        s << nonce;
        s << gasPrice;
        s << gasLimit;
        if (m_data.atKey("to").asString().size() == 42)
            s << trTo;
        else
            s << "";
        s << value;
        s << data;
        h256 hash(dev::sha3(s.out()));

        SignatureStruct sigStruct;
        if (m_data.count("secretKey"))
        {
            Signature sig = dev::sign(dev::Secret(m_data.atKey("secretKey").asString()), hash);
            sigStruct = *(SignatureStruct const*)&sig;
            ETH_FAIL_REQUIRE_MESSAGE(sigStruct.isValid(),
                TestOutputHelper::get().testName() + " Could not construct transaction signature!");
        }
        else
        {
            u256 vValue(m_data.atKey("v").asString());
            sigStruct = SignatureStruct(h256(m_data.atKey("r").asString()),
                h256(m_data.atKey("s").asString()), vValue.convert_to<byte>());
        }

        RLPStream sWithSignature;
        sWithSignature.appendList(9);
        sWithSignature << nonce;
        sWithSignature << gasPrice;
        sWithSignature << gasLimit;
        if (m_data.atKey("to").asString().size() == 42)
            sWithSignature << trTo;
        else
            sWithSignature << "";
        sWithSignature << value;
        sWithSignature << data;
        byte v = m_data.count("secretKey") ? 27 + sigStruct.v : sigStruct.v;
        sWithSignature << v;
        sWithSignature << (u256)sigStruct.r;
        sWithSignature << (u256)sigStruct.s;
        return dev::toHexPrefixed(sWithSignature.out());
    }
    };

    class scheme_generalTransaction: public object
    {
        public:
        struct transactionInfo
        {
            transactionInfo(size_t _dataInd, size_t _gasInd, size_t _valueInd, DataObject const& _transaction) :
                gasInd(_gasInd), dataInd(_dataInd), valueInd(_valueInd), executed(false), transaction(_transaction)
                {}
            size_t gasInd;
            size_t dataInd;
            size_t valueInd;
            bool executed;
            test::scheme_transaction transaction;
        };

        scheme_generalTransaction(DataObject const& _transaction):
            object(_transaction)
        {
            test::requireJsonFields(_transaction, "transaction", {
                {"data", {DataType::Array} },
                {"gasLimit", {DataType::Array} },
                {"gasPrice", {DataType::String} },
                {"nonce", {DataType::String} },
                {"secretKey", {DataType::String} },
                {"to", {DataType::String} },
                {"value", {DataType::Array} }
            });
            for (auto& element: m_data.getSubObjectsUnsafe())
            {
                if (element.getKey() == "to" && !element.asString().empty())
                {
                    element = makeHexAddress(element.asString());
                    continue;
                }
                if (element.getKey() != "data")
                    makeAllFieldsHex(element);
                else
                {
                    for (auto& element2 : element.getSubObjectsUnsafe())
                        element2 = test::replaceCode(element2.asString());
                }
            }
            parseGeneralTransaction();
        }

        std::vector<transactionInfo> const& getTransactions() const { return m_transactions; }
        std::vector<transactionInfo>& getTransactionsUnsafe() { return m_transactions; }

        private:
        std::vector<transactionInfo> m_transactions;
        void parseGeneralTransaction()
        {
            for (size_t dataInd = 0; dataInd < m_data.atKey("data").getSubObjects().size();
                 dataInd++)
            {
                for (size_t gasInd = 0; gasInd < m_data.atKey("gasLimit").getSubObjects().size();
                     gasInd++)
                {
                    for (size_t valueInd = 0;
                         valueInd < m_data.atKey("value").getSubObjects().size(); valueInd++)
                    {
                        DataObject singleTransaction(DataType::Object);
                        DataObject data(
                            "data", m_data.atKey("data").getSubObjects().at(dataInd).asString());
                        DataObject gas("gasLimit",
                            m_data.atKey("gasLimit").getSubObjects().at(gasInd).asString());
                        DataObject value(
                            "value", m_data.atKey("value").getSubObjects().at(valueInd).asString());

                        singleTransaction.addSubObject(data);
                        singleTransaction.addSubObject(gas);
                        singleTransaction.addSubObject(m_data.atKey("gasPrice"));
                        singleTransaction.addSubObject(m_data.atKey("nonce"));
                        singleTransaction.addSubObject(m_data.atKey("secretKey"));
                        singleTransaction.addSubObject(m_data.atKey("to"));
                        singleTransaction.addSubObject(value);
                        transactionInfo info(dataInd, gasInd, valueInd, singleTransaction);
                        m_transactions.push_back(info);
                    }
                }
            }
        }
    };
}

