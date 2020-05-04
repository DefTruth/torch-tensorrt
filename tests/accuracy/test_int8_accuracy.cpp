#include "accuracy_test.h"
#include "datasets/cifar10.h"
#include "torch/torch.h"

TEST_P(AccuracyTests, FP16AccuracyIsClose) {
    auto calibration_dataset = datasets::CIFAR10("tests/accuracy/datasets/data/cifar-10-batches-bin/", datasets::CIFAR10::Mode::kTest)
                                    .use_subset(320)
                                    .map(torch::data::transforms::Normalize<>({0.4914, 0.4822, 0.4465},
                                                                              {0.2023, 0.1994, 0.2010}))
                                    .map(torch::data::transforms::Stack<>());
    auto calibration_dataloader = torch::data::make_data_loader(std::move(calibration_dataset), torch::data::DataLoaderOptions()
                                                                                                    .batch_size(32)
                                                                                                    .workers(2));

    std::string calibration_cache_file = "/tmp/vgg16_TRT_ptq_calibration.cache";

    auto calibrator = trtorch::ptq::make_int8_calibrator(std::move(calibration_dataloader), calibration_cache_file, true);
    //auto calibrator = trtorch::ptq::make_int8_cache_calibrator(calibration_cache_file);


    std::vector<std::vector<int64_t>> input_shape = {{32, 3, 32, 32}};
    // Configure settings for compilation
    auto extra_info = trtorch::ExtraInfo({input_shape});
    // Set operating precision to INT8
    extra_info.op_precision = torch::kI8;
    // Use the TensorRT Entropy Calibrator
    extra_info.ptq_calibrator = calibrator;
    // Set max batch size for the engine
    extra_info.max_batch_size = 32;
    // Set a larger workspace
    extra_info.workspace_size = 1 << 28;

    mod.eval();

    // Dataloader moved into calibrator so need another for inference
    auto eval_dataset = datasets::CIFAR10("tests/accuracy/datasets/data/cifar-10-batches-bin/", datasets::CIFAR10::Mode::kTest)
                                    .use_subset(3200)
                                    .map(torch::data::transforms::Normalize<>({0.4914, 0.4822, 0.4465},
                                                                              {0.2023, 0.1994, 0.2010}))
                                    .map(torch::data::transforms::Stack<>());
    auto eval_dataloader = torch::data::make_data_loader(std::move(eval_dataset), torch::data::DataLoaderOptions()
                                                                                                    .batch_size(32)
                                                                                                    .workers(2));

    // Check the FP32 accuracy in JIT
    torch::Tensor jit_correct = torch::zeros({1}, {torch::kCUDA}), jit_total = torch::zeros({1}, {torch::kCUDA});
    for (auto batch : *eval_dataloader) {
        auto images = batch.data.to(torch::kCUDA);
        auto targets = batch.target.to(torch::kCUDA);

        auto outputs = mod.forward({images});
        auto predictions = std::get<1>(torch::max(outputs.toTensor(), 1, false));

        jit_total += targets.sizes()[0];
        jit_correct += torch::sum(torch::eq(predictions, targets));
    }
    torch::Tensor jit_accuracy = (jit_correct / jit_total) * 100;

    // Compile Graph
    auto trt_mod = trtorch::CompileGraph(mod, extra_info);

    // Check the INT8 accuracy in TRT
    torch::Tensor trt_correct = torch::zeros({1}, {torch::kCUDA}), trt_total = torch::zeros({1}, {torch::kCUDA});
    for (auto batch : *eval_dataloader) {
        auto images = batch.data.to(torch::kCUDA);
        auto targets = batch.target.to(torch::kCUDA);

        auto outputs = trt_mod.forward({images});
        auto predictions = std::get<1>(torch::max(outputs.toTensor(), 1, false));
        predictions = predictions.reshape(predictions.sizes()[0]);

        trt_total += targets.sizes()[0];
        trt_correct += torch::sum(torch::eq(predictions, targets)).item().toFloat();
    }
    torch::Tensor trt_accuracy = (trt_correct / trt_total) * 100;

    ASSERT_TRUE(trtorch::tests::util::almostEqual(jit_accuracy, trt_accuracy, 3));
}


INSTANTIATE_TEST_SUITE_P(FP16AccuracyIsCloseSuite,
                         AccuracyTests,
                         testing::Values("tests/accuracy/vgg16_cifar10.jit.pt"));