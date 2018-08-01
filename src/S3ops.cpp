#include "S3ops.hpp"
//#include "MemStreamBuf.hpp"

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/GetBucketLocationRequest.h>

#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/FileSystemUtils.h>

#include <boost/filesystem.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>

#define ALLOCATION_TAG "Orthanc S3 Storage"

namespace OrthancPlugins {

S3Facade::S3Facade(S3Method m, OrthancPluginContext *context)
{
    switch(m) {
    case S3Method::TRANSFER_MANAGER:
        _s3 = std::unique_ptr<S3Impl>(new S3TransferManager(context));
        break;
    case S3Method::DIRECT:
    default:
        _s3 = std::unique_ptr<S3Impl>(new S3Direct(context));
        break;
    }
}

bool S3Impl::ConfigureAwsSdk(const std::string& s3_access_key,  const std::string& s3_secret_key,
                               const std::string& s3_bucket_name, const std::string& s3_region) {

    //Enable AWS logging
    Aws::Utils::Logging::InitializeAWSLogging(
                Aws::MakeShared<Aws::Utils::Logging::DefaultLogSystem>( //TODO: change log system from file to std out
                    ALLOCATION_TAG, Aws::Utils::Logging::LogLevel::Info, "aws_sdk_"));

    Aws::InitAPI(aws_api_options);

    //aws_client_config.region = Aws::su
    Aws::Client::ClientConfiguration aws_client_config;
    aws_client_config.region = s3_region.c_str();
    aws_client_config.scheme = Aws::Http::Scheme::HTTPS;
    aws_client_config.connectTimeoutMs = 30000;
    aws_client_config.requestTimeoutMs = 600000;

    if (!s3_access_key.empty() && !s3_secret_key.empty()) {
        LogInfo(_context, "[S3] Using credentials from the config file");
        s3_client = Aws::MakeShared<Aws::S3::S3Client>(
                    ALLOCATION_TAG,
                    Aws::Auth::AWSCredentials(s3_access_key.c_str(), s3_secret_key.c_str()),
                    aws_client_config,
                    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, //signPayloads
                    false //useVirtualAdressing
                    );

    } else {
        LogInfo(_context, "No credentials in the config file. Falling back to ~/.aws/credentials or env variables.");
        s3_client = Aws::MakeShared<Aws::S3::S3Client>(ALLOCATION_TAG,
                                                       aws_client_config,
                                                       Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                                                       false //useVirtualAdressing
                                                       );
    }

    _bucket_name = s3_bucket_name.c_str();

    std::stringstream ss;
    ss <<  "[S3] Checking bucket: " << _bucket_name;
    LogInfo(_context, ss.str().c_str());

    //Create bucket if it doesn't exist
    //and verify if it exists
    Aws::S3::Model::CreateBucketRequest request;
    request.SetBucket(_bucket_name);
    Aws::S3::Model::CreateBucketConfiguration req_config;
    req_config.SetLocationConstraint(Aws::S3::Model::BucketLocationConstraintMapper::GetBucketLocationConstraintForName(s3_region.c_str()));
    request.SetCreateBucketConfiguration(req_config);

    auto outcome = s3_client->CreateBucket(request);

    if (outcome.GetError().GetErrorType() == Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU ||
            outcome.GetError().GetErrorType() == Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS ) {
        std::stringstream ss;
        ss << "[S3] Bucket exists: " << _bucket_name;
        LogInfo(_context, ss.str().c_str());
    } else if (outcome.IsSuccess()) {
        std::stringstream ss;
        ss << "[S3] Bucket created: " << _bucket_name;
        LogInfo(_context, ss.str().c_str());
    } else {
        std::stringstream err;
        err << "[S3] Create Bucket error: " <<
               outcome.GetError().GetExceptionName() << " " <<
               outcome.GetError().GetMessage();
        LogError(_context, err.str().c_str());
        return false;
    }

    return true;
}

bool S3Direct::UploadFileToS3(const std::string &path, const void *content, const int64_t &size) {
    const Aws::String key_name = path.c_str();
    const Aws::String file_name = path.c_str();

    Aws::S3::Model::PutObjectRequest object_request;
    object_request.WithBucket(_bucket_name).WithKey(key_name);

    boost::interprocess::bufferstream buf(const_cast<char*>(static_cast<const char*>(content)), static_cast<size_t>(size));
    auto body = Aws::MakeShared<Aws::IOStream>(ALLOCATION_TAG, buf.rdbuf());

    //std::stringbuf buf(const_cast<char*>(static_cast<const char*>(content)), size);
    //std::shared_ptr<Aws::IOStream> body = Aws::MakeShared<Aws::IOStream>(ALLOCATION_TAG, &buf);

    auto input_data = Aws::MakeShared<Aws::FStream>("PutObjectInputStream",
                                                    file_name.c_str(), std::ios_base::in | std::ios_base::binary);

    object_request.SetBody(body);

    auto put_object_outcome = s3_client->PutObject(object_request);

    if (!put_object_outcome.IsSuccess()) {
        std::stringstream err;
        err << "[S3] PUT error: " <<
               put_object_outcome.GetError().GetExceptionName() << " " <<
               put_object_outcome.GetError().GetMessage();
        LogError(_context, err.str().c_str());

        return false;
    }

    return true;

}

bool S3Direct::DownloadFileFromS3(const std::string &path, void **content, int64_t *size) {
    const Aws::String key_name = path.c_str();

    Aws::S3::Model::GetObjectRequest object_request;
    object_request.WithBucket(_bucket_name).WithKey(key_name);

    auto get_object_outcome = s3_client->GetObject(object_request);

    if (get_object_outcome.IsSuccess()) {
        *size = get_object_outcome.GetResult().GetContentLength();

        Aws::OStringStream buf;
        //malloc because it's freed by ::free()
        *content = malloc(static_cast<size_t>(*size));

        if (*content!=nullptr) {
            buf.rdbuf()->pubsetbuf(static_cast<char*>(*content), *size);
            buf << get_object_outcome.GetResult().GetBody().rdbuf();

            //memcpy(*content, buf.str().c_str(), *size);
        } else {
            return false;
        }
    } else {
        std::stringstream err;
        err << "[S3] GET error: " <<
               get_object_outcome.GetError().GetExceptionName() << " " <<
               get_object_outcome.GetError().GetMessage();
        LogError(_context, err.str().c_str());

        return false;
    }

    return true;
}

bool S3Direct::DeleteFileFromS3(const std::string &path) {
    const Aws::String key_name = path.c_str();

    Aws::S3::Model::DeleteObjectRequest object_request;
    object_request.WithBucket(_bucket_name).WithKey(key_name);

    auto delete_object_outcome = s3_client->DeleteObject(object_request);

    if (!delete_object_outcome.IsSuccess()) {
        std::stringstream err;
        err << "[S3] DELETE error: " <<
               delete_object_outcome.GetError().GetExceptionName() << " " <<
               delete_object_outcome.GetError().GetMessage();
        LogError(_context, err.str().c_str());

        return false;
    }

    return true;
}

/*
 * Transfer Manager Implementation
 */

void S3TransferManager::LogDetails(const std::shared_ptr<const Aws::Transfer::TransferHandle>& req) {
    std::stringstream ss;

    ss << "Status: ";
    switch (req->GetStatus()) {
    case Aws::Transfer::TransferStatus::ABORTED: ss << "Aborted"; break;
    case Aws::Transfer::TransferStatus::CANCELED: ss << "Canceled"; break;
    case Aws::Transfer::TransferStatus::COMPLETED: ss << "Completed"; break;
    case Aws::Transfer::TransferStatus::EXACT_OBJECT_ALREADY_EXISTS: ss << "Already Exists"; break;
    case Aws::Transfer::TransferStatus::FAILED: ss << "Failed"; break;
    case Aws::Transfer::TransferStatus::IN_PROGRESS: ss << "In Progress"; break;
    case Aws::Transfer::TransferStatus::NOT_STARTED: ss << "Not Started"; break;
    default:
        break;
    }
    ss << ". ";
    ss << "completed: " << req->GetCompletedParts().size(); // Should be tiny
    ss << ", failed: " << req->GetFailedParts().size();
    ss << ", pending: " << req->GetPendingParts().size();
    ss << ", queued: " << req->GetQueuedParts().size();

    LogInfo(_context, ss.str().c_str());
}

bool S3TransferManager::ConfigureAwsSdk(const std::string &s3_access_key, const std::string &s3_secret_key, const std::string &s3_bucket_name, const std::string &s3_region) {

    S3Impl::ConfigureAwsSdk(s3_access_key, s3_secret_key, s3_bucket_name, s3_region);

    _executor = Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(ALLOCATION_TAG, 4);
    Aws::Transfer::TransferManagerConfiguration transferConfig(_executor.get());
    transferConfig.s3Client = s3_client;

    transferConfig.errorCallback = [&](const Aws::Transfer::TransferManager*, const std::shared_ptr<const Aws::Transfer::TransferHandle>& req, const Aws::Client::AWSError<Aws::S3::S3Errors>& e) {
        std::stringstream ss;
        ss << "[S3] Error: " << e.GetMessage() << '.';

        LogDetails(req);
    };

    _tm = Aws::Transfer::TransferManager::Create(transferConfig);

    return true;
}

bool S3TransferManager::UploadFileToS3(const std::string &path, const void *content, const int64_t &size) {
    boost::interprocess::bufferstream buf(const_cast<char*>(static_cast<const char*>(content)), static_cast<size_t>(size));
    auto body = Aws::MakeShared<Aws::IOStream>(ALLOCATION_TAG, buf.rdbuf());

    auto requestPtr = _tm->UploadFile(body,
                                      _bucket_name,
                                      path.c_str(),
                                      "text/plain",
                                      Aws::Map<Aws::String, Aws::String>());

    requestPtr->WaitUntilFinished();

    size_t retries = 0;
    while (requestPtr->GetStatus() != Aws::Transfer::TransferStatus::COMPLETED && retries++ < 5)
    {
        _tm->RetryUpload(body, requestPtr);
        requestPtr->WaitUntilFinished();
    }

    LogDetails(requestPtr);

    return (requestPtr->GetStatus() == Aws::Transfer::TransferStatus::COMPLETED); 
}

bool S3TransferManager::DownloadFileFromS3(const std::string &path, void **content, int64_t *size) {

    boost::filesystem::path temp = "/tmp" / boost::filesystem::unique_path();
    const std::string tempstr    = temp.native();  // optional

    {
        std::stringstream ss;
        ss << "[S3] Using tmp: " << tempstr << '.';
        LogInfo(_context, ss.str());
    }

    auto requestPtr = _tm->DownloadFile(_bucket_name,
                                        path.c_str(),
                                        tempstr.c_str());

    requestPtr->WaitUntilFinished();

    if (requestPtr->GetStatus() == Aws::Transfer::TransferStatus::COMPLETED) {
       //read file to memory
        std::ifstream file(tempstr, std::ios::binary | std::ios::ate);
        *size = file.tellg();
        file.seekg(0, std::ios::beg);

        *content = malloc(*size);
        if (!*content) {
            std::stringstream ss;
            ss << "[S3] Could not allocate: " << *size << " bytes.";
            LogError(_context, ss.str());

           return false;
        }

        if (!file.read(static_cast<char*>(*content), *size))
        {
            std::stringstream ss;
            ss << "[S3] Failed to read file: " << tempstr << ".";
            LogError(_context, ss.str());

           return false;
        }
    } else {
        std::stringstream ss;
        auto err = requestPtr->GetLastError();
        ss << "[S3] Failed to get file: " << path << " because of: " << err.GetMessage() <<'.';
        LogError(_context, ss.str());
    }

    std::remove(tempstr.c_str());

    return (requestPtr->GetStatus() == Aws::Transfer::TransferStatus::COMPLETED);


}

/*
 * TODO: fix the memory transfer
bool S3TransferManager::DownloadFileFromS3(const std::string &path, void **content, int64_t *size) {

    auto buf = Aws::MakeShared<Stream::MemStreamBuf>(ALLOCATION_TAG, nullptr, 0, false);
    auto oss = std::make_shared<std::istream>(buf.get());

    auto requestPtr = _tm->DownloadFile(_bucket_name,
                                        path.c_str(),
                                        [=](){
        auto* out = Aws::New<Aws::IOStream>(ALLOCATION_TAG, buf.get());

        LogInfo(_context, "Creating stream");
        return out;
    });

    requestPtr->WaitUntilFinished();

    LogInfo (_context, "Finished");

    if (requestPtr->GetStatus() == Aws::Transfer::TransferStatus::COMPLETED) {
        LogDetails (requestPtr);

        //assert(buf->size() == requestPtr->GetBytesTotalSize());
        size_t s1 = buf->size();
        size_t s2 = buf->allocsize();
        size_t s3 = requestPtr->GetBytesTransferred();
        size_t s4 = requestPtr->GetBytesTotalSize();
        *size = requestPtr->GetBytesTotalSize();

        //oss->seekp(0, std::ios::end);
        // *size = oss->tellp();
        //oss->seekp(0);//, std::ios::beg);

        std::stringstream ss;
        ss << "Size: " << *size << '\n';
        ss << "s1: " << s1 << '\n';
        ss << "s2: " << s2 << '\n';
        ss << "s3: " << s3 << '\n';
        ss << "s4: " << s4 << '\n';
        LogInfo(_context, ss.str().c_str());

        *content = buf->get();

        if (*content == nullptr) {
            LogError(_context, "Error allocating memory");
            return false;
        }

        //LogInfo(_context, "Copying memory");
        //oss->rdbuf()->sgetn(static_cast<char*>(*content), *size);
        LogInfo(_context, "DONE");
    }

    return (requestPtr->GetStatus() == Aws::Transfer::TransferStatus::COMPLETED);
}
*/

bool S3TransferManager::DeleteFileFromS3(const std::string &path) {
    const Aws::String key_name = path.c_str();

    Aws::S3::Model::DeleteObjectRequest object_request;
    object_request.WithBucket(_bucket_name).WithKey(key_name);

    auto delete_object_outcome = s3_client->DeleteObject(object_request);

    if (!delete_object_outcome.IsSuccess()) {
        std::stringstream err;
        err << "[S3] DELETE error: " <<
               delete_object_outcome.GetError().GetExceptionName() << " " <<
               delete_object_outcome.GetError().GetMessage();
        LogError(_context, err.str().c_str());

        return false;
    }

    return true;
}


}
