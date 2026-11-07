
namespace {
uint32_t record_number=0;
void clear_test_record() { record_number=0; }
bool take_test_record(const tsuchi::Json& root,const char*) {
    const tsuchi::Json* value=root.find("number");
    if(!value || value->kind!=tsuchi::Json::Kind::number || !value->whole || value->integer<=0 || value->integer>1000) return false;
    record_number=uint32_t(value->integer);
    return true;
}
void put_test_record(tsuchi::Written& out) { out.add(", \"number\": ").number(record_number); }
const char* const test_record_keys[]={"number"};
tsuchi::Record test_record{"checks", "save:/tsuchi/old_checks.json",test_record_keys,1,take_test_record,put_test_record,clear_test_record};
}
void records_checks() {
    using tsuchi::read_record;
    using tsuchi::write_record;
    const auto before=disk.files;
    const std::string at="save:/tsuchi/tsuchi.content/checks.json",second="save:/tsuchi/tsuchi.content/checks.2.json";
    tsuchi::forget_records();
    disk.files[at]=R"({"counter": 1, "number": 5, "future": {"escaped": "a\"b", "list": [true, null, 42]}})";
    disk.files[second]=R"({"counter": 2, "number":)";
    CHECK(read_record(test_record)==TSUCHI_OK && record_number==5 && test_record.counter==1);
    // A process cache keeps whose progress the number identifies despite changes to the save.
    disk.files[at]=R"({"counter": 3, "number": 7})";
    CHECK(read_record(test_record)==TSUCHI_OK && record_number==5);
    record_number=6;
    CHECK(write_record(test_record)==TSUCHI_OK && test_record.counter==2);
    CHECK(disk.files[second].find(R"("future": {"escaped": "a\"b", "list": [true, null, 42]})")!=std::string::npos);
    const std::string durable=disk.files[second];
    disk.refuse="write",record_number=8;
    CHECK(write_record(test_record)==TSUCHI_IO && disk.files[second]==durable && test_record.counter==2);
    disk.refuse.clear();
    // Corrupt the other copy too. Both damaged blocks and cannot erase the previous numbers.
    tsuchi::forget_records();
    disk.files[at]="{",disk.files[second]="{";
    CHECK(read_record(test_record)==TSUCHI_INVALID && write_record(test_record)==TSUCHI_NOT_READY);
    CHECK(disk.files[at]=="{" && disk.files[second]=="{");
    disk.files[at]=R"({"counter": 1, "number": 5})";
    disk.files[second]=R"({"counter": 2, "number": false})";
    tsuchi::forget_records();
    CHECK(read_record(test_record)==TSUCHI_OK && record_number==5 && test_record.counter==1);
    // Legacy migration writes one new copy and keeps the original.
    disk.files.erase(at),disk.files.erase(second);
    disk.files["save:/tsuchi/old_checks.json"]=R"({"number": 9, "future": "keep"})";
    tsuchi::forget_records();
    CHECK(read_record(test_record)==TSUCHI_OK && record_number==9);
    CHECK(write_record(test_record)==TSUCHI_OK && disk.files[at].find("\"number\": 9")!=std::string::npos);
    CHECK(disk.files["save:/tsuchi/old_checks.json"]==R"({"number": 9, "future": "keep"})");
    tsuchi::forget_records();
    disk.files=before,disk.changes.clear();
}
