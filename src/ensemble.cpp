#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>

#include <tclap/CmdLine.h>
#include <json.hpp>
#include <casebase.cpp>
#include <io.cpp>
#include <utils.cpp>
#include <experimental/filesystem>

template<class T, class U>
T get_param(const U& p, T def) {
    try {
        return T(p);
    } catch (...) {
        return def;
    } 
}

int main(int argc, char** argv)
{
    using std::cerr;
    using std::cout;
    using std::endl;
    using std::size;
    using std::string;
    using std::vector;
    using json = nlohmann::json;
    namespace  fs = std::experimental::filesystem;

    TCLAP::CmdLine cmd("Hypergraph Case-Base Reasoner", ' ', "0.0.1");
    TCLAP::ValueArg<string> param_file_arg("", "params","JSON parameter file (c.f. documentation for examples)", false, "./params.json", "string", cmd);

    cmd.parse(argc, argv);
    std::fstream log;

    // TODO: Check if the location exists, and if the parameter file is correct
    auto param_file_path = param_file_arg.getValue();
    auto params = load_and_validate_parameters(param_file_path);

    // 1. DATA
    const auto deserialize = params["deserialization"]["deserialize"];
    const std::string deserialize_path = params["deserialization"]["path"];
    fs::path deserialize_p(deserialize_path);
    if (deserialize and not fs::exists(deserialize_p)) {
        cerr << "Deserialization path does not exist." << endl;
        return 3;
    }

    const auto serialize = params["serialization"]["serialize"];
    const std::string serialize_path = params["serialization"]["path"];

    fs::path serialize_p(serialize_path);
    if (serialize and not fs::exists(serialize_p)) {
        fs::create_directories(serialize_path);
    }

    const auto mu0_path = params["serialization"]["mu0_file"];
    const auto mu1_path = params["serialization"]["mu1_file"];
    auto mu1 = vector<double>();
    auto mu0 = vector<double>();
    try {
        std::cerr << mu0_path << " " << mu1_path << std::endl;
        mu1 = read_vector(mu1_path);
        mu0 = read_vector(mu0_path);
    } catch (std::exception &e)  // catch any exceptions
    {
        cerr << "Error: " << e.what() << endl;
        return 3;
    }

    // 1.1 CMD verification
    const auto delta = float(params["hyperparameters"]["delta"]);
    const auto gamma = float(params["hyperparameters"]["gamma"]);
    //if(delta < -1 || delta > 1)
    //    throw std::domain_error("Delta must belong to [-1,1]");
    auto bias = float(params["hyperparameters"]["bias"]);
    auto auto_bias = get_param<bool>(params["hyperparameters"]["auto_bias"], false);
    const auto eta1 = float(params["hyperparameters"]["eta1"]);
    const auto eta0 = float(params["hyperparameters"]["eta0"]);
    const auto bar_eta1 = float(params["hyperparameters"]["bar_eta1"]);
    const auto bar_eta0 = float(params["hyperparameters"]["bar_eta0"]);
    const auto l1 = int(params["hyperparameters"]["l1"]);
    const auto l0 = int(params["hyperparameters"]["l0"]);

    const auto casebase_file = params["input"]["casebase"];
    const auto outcomes_file = params["input"]["outcomes"];
    const auto features_file = params["input"]["features"];

    std::cerr << "# Loading the instance files..." << std::endl;

    auto cases = vector<vector<int>>();
    auto outcomes = vector<int>();
    auto features = std::map<int, string>();
    try {
        cases = read_case_base(casebase_file);
        outcomes = read_mapping(outcomes_file);
        features = read_features(features_file);
        if(size(cases) == 0)
            throw std::domain_error("# The casebase file could not be found or is empty.");
        if(size(outcomes) == 0)
            throw std::domain_error("# The outcomes file could not be found or is empty.");
        if(size(cases) != size(outcomes))
            throw std::domain_error("# The outcomes and casebase sizes are different.");
    } catch (std::exception &e)  // catch any exceptions
    {
        cerr << "Error: " << e.what() << endl;
        return 3;
    }

    cerr << "# Perform sanity checks on the dataset" << endl;
    data_sanity_check(cases, outcomes); // TODO: React on it

    std::cerr << "# Setting the parameters..." << std::endl;
    auto nb_models = get_param<int>(params["parameters"]["models"], 1);
    auto examples_per_model = get_param<double>(params["parameters"]["examples_per_model"], 1.); // in % of available training set
    auto max_learning_iterations = int(params["parameters"]["training_iterations"]);
    auto online = bool(params["parameters"]["online"]);
    auto verbose = int(params["output"]["verbose"]);
    auto starting_case = int(params["parameters"]["starting_case"]);
    auto sample_out =  bool(params["parameters"]["sample_out"]);
    auto keep_offset = bool(params["parameters"]["keep_offset"]);
    auto limit_examples = int(params["parameters"]["limit"]) + starting_case;
    auto run_id = params["parameters"]["run_id"];
    auto check_if_in_cb = bool(params["parameters"]["heuristic"]);
    auto no_pred = bool(params["parameters"]["no_prediction"]);
    if(limit_examples > size(cases)) {
        cout << "# The limit is larger than the cases in the casebase. It will be set to the casebase size." << endl;
        limit_examples = size(cases);
    }
    else if(limit_examples == -1) {
        limit_examples = size(cases);
    }
    if(sample_out && limit_examples == size(cases)) {
        cout << "# Disable the Sample Out feature due to the limit parameter being as large as the casebase." << endl;
    }

    std::cerr << "# Initialize..." << std::endl;

    // 1.2 Number of features detection
    auto n_cases = size(cases);
    auto feature_map = features_count(cases);
    auto total_features = total_features_count(feature_map);
    decltype(cases)::iterator min_e, max_e;
    std::tie(min_e, max_e) = std::minmax_element(begin(cases), end(cases),
                            [](const vector<int>& v1, const vector<int>& v2) {
                                 return size(v1) < size(v2);
                             });
    auto avg_features = std::accumulate(begin(cases), end(cases), 0,
                            [](int& r, const vector<int>& v) {
                                 return r + size(v);
                             }) / double(n_cases);
    cerr << "# Online: " << online << endl;
    cerr << "# Verbose level: " << verbose << endl;
    cerr << "# Cases: " << n_cases << endl;
    cerr << "# Total features: " << total_features << endl;
    cerr << "# Unique features: " << size(feature_map) << " (ratio: " << size(feature_map) / double(total_features) << ")" << endl;
    cerr << "# Minimum case size: " << size(*min_e) << endl;
    cerr << "# Maximum case size: " << size(*max_e) << endl;
    cerr << "# Average case size: " << avg_features << endl;

    // 2. Create the necessary variables
    auto avr_good = 0.;
    auto total_time = 0.;
    auto nc = cases[0];
    auto o = outcomes[0];
    std::pair<std::map<int, std::vector<int>>, std::vector<int>> proj;
    auto prediction = 0;
    auto pred_0 = 0.;
    auto pred_1 = 0.;
    auto r = 0.;
    auto rdf = 0.;
    auto avr_good_0 = 0.;
    auto avr_good_1 = 0.;
    auto avg_diff_bad_0 = 0.;
    auto avg_diff_bad_1 = 0.;
    auto nb_bad_0 = 0;
    auto nb_bad_1 = 0;
    auto nb_good_0 = 0;
    auto nb_good_1 = 0;
    std::tuple<double, double> pred;

    auto log_run = std::string{};
    if (!std::ifstream(log_file_name("hcbr", -1))) {
        log_run += "Total Cases, \
                 Total Features, \
                 Unique Features, \
                 Min. Case Size, \
                 Max. Case Size, \
                 Avg. Case Size, \
                 Building Time, \
                 Examples, \
                 Unique Features in Examples, \
                 Unique Feature Ratio in Examples, \
                 Cardinal Partition, \
                 Min. Card. Part. Element, \
                 Max. Card. Part. Element, \
                 Avg. Card. Part. Element, \
                 Min. |v|, \
                 Max. |v|, \
                 Avg. |v|, \
                 Min. |e| per case, \
                 Max. |e| per case, \
                 Avg. |e| per case, \
                 Min. |e| per case (%), \
                 Max. |e| per case (%), \
                 Avg. |e| per case (%), \
                 Min. DFR in Examples, \
                 Max. DFR in Examples, \
                 Avg. DFR in Examples, \
                 Min. DFR in Examples (%), \
                 Max. DFR in Examples (%), \
                 Avg. DFR in Examples (%), \
                 Strength Time, \
                 Learning Time, \
                 Learning Phases, \
                 Learning Accuracy, \
                 Learning True Positive, \
                 Learning False Positive, \
                 Learning False Negative, \
                 Learning True Negative, \
                 Prediction Time, \
                 Cardinal Prediction, \
                 Total Time, \
                 Min. DFR in Prediction, \
                 Max. DFR in Prediction, \
                 Avg. DFR in Prediction, \
                 Min. DFR in Prediction (%), \
                 Max. DFR in Prediction (%), \
                 Avg. DFR in Prediction (%), \
                 Prediction Accuracy, \
                 True Positive, \
                 False Positive, \
                 False Negative, \
                 True Negative, \
                 True Positive Rate (sensitivity), \
                 True Negative Rate (specificity), \
                 Positive Prediction Value (precision), \
                 Negative Prediction Value, \
                 False Negative Rate (miss rate), \
                 False Positive Rate (fall-out), \
                 False Discovery Rate, \
                 False Omission Rate, \
                 F1 Score, \
                 Matthews Correlation Coefficient, \
                 Avg. Diff. True Pos., \
                 Avg. Diff. True Neg., \
                 Avg. Diff. False Pos., \
                 Avg. Diff. False Neg., \
                 Avg. Diff. True Pos. (%), \
                 Avg. Diff. True Neg. (%), \
                 Avg. Diff. False Pos. (%), \
                 Avg. Diff. False Neg. (%), \
                 Ratio Cases already in CB , \
                 \n";
    }
    log_run += std::to_string(n_cases) + " , " 
             + std::to_string(total_features) + " , " 
             + std::to_string(size(feature_map)) + " , "
             + std::to_string(size(*min_e)) + " , "
             + std::to_string(size(*max_e)) + " , "
             + std::to_string(avg_features) + " , ";

    auto log_training = std::string{};
    if (!std::ifstream(log_file_name("training", run_id))) {
        log_training += "Time, \
                 Accuracy, \
                 True Positive, \
                 False Positive, \
                 False Negative, \
                 True Negative, \
                 Avg. Diff. True Pos., \
                 Avg. Diff. True Neg., \
                 Avg. Diff. False Pos., \
                 Avg. Diff. False Neg., \
                 Avg. Diff. True Pos. (%), \
                 Avg. Diff. True Neg. (%), \
                 Avg. Diff. False Pos. (%), \
                 Avg. Diff. False Neg. (%), \
                 MCC\n";
        log.open(log_file_name("training", run_id), std::fstream::in | std::fstream::out | std::fstream::app);
        log << log_training << endl;
        log.close();
    }
    auto log_prediction = std::string{};
    if (!std::ifstream(log_file_name("prediction", run_id))) {
        log_prediction += "Time, \
                 Total Time, \
                 Case Size, \
                 Outcome, \
                 Prediction, \
                 Cumulated Good Predictions, \
                 Accuracy, \
                 Score 1, \
                 Score 0, \
                 Cardinal Partition, \
                 RDF, \
                 RDF (%), \
                 True Positive, \
                 False Positive, \
                 False Negative, \
                 True Negative, \
                 Avg. Diff. True Pos., \
                 Avg. Diff. True Neg., \
                 Avg. Diff. False Pos., \
                 Avg. Diff. False Neg., \
                 Avg. Diff. True Pos. (%), \
                 Avg. Diff. True Neg. (%), \
                 Avg. Diff. False Pos. (%), \
                 Avg. Diff. False Neg. (%)";
        log.open(log_file_name("prediction", run_id), std::fstream::in | std::fstream::out | std::fstream::app);
        log << log_prediction << endl;
        log.close();
    }

    // 3. Initialize the random generator
    std::random_device rnd_device;
    std::mt19937 gen(rnd_device());
    auto seed = params["parameters"]["seed"];
    if(seed == 0)
        seed = std::time(0);
    std::srand(seed);

    auto indexes = vector<int>(size(cases));
    std::iota(begin(indexes), end(indexes), 0);
    auto shuffle = bool(params["parameters"]["shuffle"]);
    if(shuffle) {
        cerr << "# Shuffle the casebase..." << endl;
        std::random_shuffle(begin(indexes), end(indexes));
    }


    vector<CaseBase> models;
    cerr << "# Create " << nb_models << " models..." << endl;
    for(int i = 0; i < nb_models; ++i) {
        models.push_back(CaseBase(size(feature_map), n_cases));
    }

    auto start_global_time = std::chrono::steady_clock::now();
    auto start_time = std::chrono::steady_clock::now();
    std::default_random_engine generator;
    auto nb_examples = int((limit_examples - starting_case) * examples_per_model);
    std::uniform_int_distribution<int> distribution(starting_case, limit_examples);
    cerr << "# Add " << nb_examples << " random cases per model..." << endl;
    for(int m = 0; m < nb_models; ++m) {
        //cerr << "MODEL " << m << endl;
        auto r_i = vector<int>(limit_examples - starting_case);
        std::iota(begin(r_i), end(r_i), 0);
        std::random_shuffle(begin(r_i), end(r_i));
        for(auto i = 0; i < nb_examples; ++i) {
            int j = r_i[i];//distribution(generator);
            //cerr << "C" << indexes[j] << " | O = " << outcomes[indexes[j]] << endl;
            o = outcomes[indexes[j]];
            nc = cases[indexes[j]];
            models[m].add_case(nc, o, false);//online);
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto diff = end_time - start_time;
    auto time = std::chrono::duration<double, std::ratio<1, 1>>(diff).count();

    cerr << "# Calculate intrinsic strength..." << endl;
    //if (not deserialize) 
    {
        start_time = std::chrono::steady_clock::now();
        for(int m = 0; m < nb_models; ++m) {
            cerr << " - Model " << m << endl;
            models[m].calculate_strength(log, run_id);
        }
        end_time = std::chrono::steady_clock::now();
        diff = end_time - start_time;
        time = std::chrono::duration<double, std::ratio<1, 1>>(diff).count();
        log_run += std::to_string(time) + " , ";
    }

    cerr << "# Learning phase..." << endl;
    auto offset_0 = 0.;
    auto offset_1 = 0.;
    for(int m = 0; m < nb_models; ++m) {
            auto epsilon = 0.;
        auto accuracy = 0.;
        auto avg_diff_bad_0_pct = 0.;
        auto avg_diff_bad_1_pct = 0.;
        avr_good = 0.;
        auto avg_diff_good_0 = 0.;
        auto avg_diff_good_1 = 0.;
        auto avg_diff_good_0_pct = 0.;
        auto avg_diff_good_1_pct = 0.;
        auto tp = 0;
        auto tn = 0;
        auto fp = 0;
        auto fn = 0;

        std::ofstream pred_outfile; 
        pred_outfile.open("training_set_prediction_post_training.txt", std::ofstream::out | std::ofstream::trunc);
        pred_outfile << "index correct pred s_1 s_0" << endl;
        log.open(log_file_name("training", run_id), std::fstream::in | std::fstream::out | std::fstream::app);
        for(auto iter = -1; iter < max_learning_iterations + 1; ++iter) 
        {
            auto start_time = std::chrono::steady_clock::now();
            if(iter ==  max_learning_iterations) {
                cerr << " - Verification" << endl;
            } else if (iter == -1)
            {
                cerr << " - Before training" << endl;
            } else
                cerr << " - Phase " << iter + 1 << endl;
            avg_diff_good_0 = 0.;
            avg_diff_good_1 = 0.;
            avg_diff_good_0_pct = 0.;
            avg_diff_good_1_pct = 0.;
            tp = 0;
            tn = 0;
            fp = 0;
            fn = 0;
            log_training = "";
            for(auto i = 0; i < size(models[m].outcomes); ++i) 
            {
                o = models[m].outcomes[i];
                nc = models[m].cases[i];
                proj = models[m].projection(nc);
                rdf = std::size(proj.second) / double(std::size(nc));

                auto non_disc_features = double(std::size(nc));
                pred_0 = 0.;
                pred_1 = 0.;
                for(const auto& k: proj.first) {
                    r = size(models[m].intersection_family[k.first]) / double(non_disc_features);
                    pred_0 += r * models[m].e_intrinsic_strength[0][k.first];
                    pred_1 += r * models[m].e_intrinsic_strength[1][k.first];
                }
                pred = normalize_prediction(pred_0, pred_1, 0, 0, 0);// eta, delta, offset_0, offset_1);// avg_diff_bad_0 / (i+1), avg_diff_bad_1 / (i+1));
                prediction = prediction_rule(pred, rdf, 0, 0, 0, 0, 0, 0, 1, 0, gen);// gamma, eta, gen);

                if(iter >= max_learning_iterations)
                    pred_outfile << std::setprecision(15) << indexes[i] << " " << o << " " <<  prediction << " " << pred_1 << " " << pred_0 << std::endl;

                avr_good += 1 - abs(o - prediction);
                if(prediction == 1)
                    if(o - prediction != 0) {
                        fp += 1;
                        avg_diff_bad_1 += std::get<1>(pred) - std::get<0>(pred);
                        avg_diff_bad_1_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                    }
                    else {
                        tp += 1;
                        avg_diff_good_1 += std::get<1>(pred) - std::get<0>(pred);
                        avg_diff_good_1_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                    }
                if(prediction == 0)
                    if (o - prediction != 0) {
                        fn += 1;
                        avg_diff_bad_0 += std::get<1>(pred) - std::get<0>(pred);
                        avg_diff_bad_0_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                    }
                    else {
                        tn += 1;
                        avg_diff_good_0 += std::get<1>(pred) - std::get<0>(pred);
                        avg_diff_good_0_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                    }
                if(iter > -1 and iter < max_learning_iterations) {
                    if(abs(o - prediction) != 0) {
                        for(const auto& k: proj.first) {
                            if(prediction == 1) {
                                r = size(models[m].intersection_family[k.first]) / double(non_disc_features);
                                auto err = abs(models[m].e_intrinsic_strength[0][k.first] - models[m].e_intrinsic_strength[1][k.first]);
                                models[m].e_intrinsic_strength[0][k.first] += r * err;
                                models[m].e_intrinsic_strength[1][k.first] -= r * err;
                            } else {

                                r = size(models[m].intersection_family[k.first]) / double(non_disc_features);
                                auto err = abs(models[m].e_intrinsic_strength[0][k.first] - models[m].e_intrinsic_strength[1][k.first]);
                                models[m].e_intrinsic_strength[0][k.first] -= r * err;
                                models[m].e_intrinsic_strength[1][k.first] += r * err;
                            }

                        }
                    }
                }
            }
            pred_outfile.close();
            auto end_time = std::chrono::steady_clock::now();
            auto diff = end_time - start_time;
            auto time = std::chrono::duration<double, std::ratio<1, 1>>(diff).count();
            offset_0 = 0;//-avg_diff_bad_0 / (limit_examples - starting_case + 1) * 1;//prev;
            offset_1 = 0;//avg_diff_bad_1 / (limit_examples - starting_case + 1) * 1;//(1. - prev);
            cerr << "Accuracy: " << (tp + tn) << "/" << (tp + tn + fn + fp) << " = " << (tp + tn) / double(tp + tn + fn + fp) << endl;
            cerr << "Average error toward 0: " << avg_diff_bad_0 / (limit_examples - starting_case + 1) << " (" << fn << ")" << endl;
            cerr << "Average error toward 1: " << avg_diff_bad_1 / (limit_examples - starting_case + 1) << " (" << fp << ")" << endl;
            //cerr << "Prev: " << prev / (limit_examples - starting_case) << " (error: " << double(fn) / (limit_examples - starting_case) << ") Offset: " <<  offset_0 << " " << offset_1 << endl;
            cerr << "Ratio error 1 : " << double(fp) / (fn + fp) << endl;

            cerr << "-----------------" << endl;
            cerr << "- " << std::fixed << tp << " - " << fp << " - " << endl;
            cerr << "-----------------" << endl;
            cerr << "- " << std::fixed << fn << " - " << tn << " - " << endl;
            cerr << "-----------------" << endl;

            log << std::to_string(time) << " , "
                << (tp + tn) / double(tp + tn + fn + fp) << " , "
                << std::to_string(tp) << " , "
                << std::to_string(tn) << " , "
                << std::to_string(fp) << " , "
                << std::to_string(fn) << " , "
                << std::to_string(avg_diff_good_1 / tp) << " , "
                << std::to_string(avg_diff_good_0 / tn) << " , "
                << std::to_string(avg_diff_bad_1 / fp) << " , "
                << std::to_string(avg_diff_bad_0 / fn) << " , "
                << std::to_string(100 * avg_diff_good_1_pct / tp) << " , "
                << std::to_string(100 * avg_diff_good_0_pct / tn) << " , "
                << std::to_string(100 * avg_diff_bad_1_pct / fp) << " , "
                << std::to_string(100 * avg_diff_bad_0_pct / fn) << " , "
                << std::to_string(((long long int)(tp * tn) - (fp * fn)) / sqrt((long long int)(tp + fp)*(tp + fn)*(tn + fp)*(tn + fn)))  + " , "
                << std::endl;
        }
    }




    auto j = 0;
    // Reset for prediction
    avg_diff_bad_0 = 0.;
    avg_diff_bad_1 = 0.;
    auto avg_diff_bad_0_pct = 0.;
    auto avg_diff_bad_1_pct = 0.;
    avr_good = 0.;
    auto avg_diff_good_0 = 0.;
    auto avg_diff_good_1 = 0.;
    auto avg_diff_good_0_pct = 0.;
    auto avg_diff_good_1_pct = 0.;
    auto tp = 0;
    auto tn = 0;
    auto fp = 0;
    auto fn = 0;
    auto min_toward_0 = 100000;
    auto min_toward_1 = 100000;
    auto already_in_cb = 0;
    auto already_in_cb_good = 0;

    auto dfr_exists = false;
    auto min_dfr = total_features;
    auto max_dfr = 0;
    auto avg_dfr = 0;
    auto min_dfr_pct = 1.;
    auto max_dfr_pct = 0.;
    auto avg_dfr_pct = 0.;

    auto accuracy = 0.;
    //pred_outfile.open("predictions.txt", std::ofstream::out | std::ofstream::trunc);
    //pred_outfile << "index correct pred s_1 s_0" << endl;

    if(!no_pred) {
        cerr << "# Predictions" << endl;
        log.open(log_file_name("prediction", run_id), std::fstream::in | std::fstream::out | std::fstream::app);
        start_time = std::chrono::steady_clock::now();
        for(auto i = limit_examples+1; i < n_cases; ++i) {
            log_prediction = "";
            auto start_iteration = std::chrono::steady_clock::now();
            o = outcomes[indexes[i]];
            nc = cases[indexes[i]];

            // GENERATE A PREDICTION FOR EACH MODEL
            vector<std::tuple<double, double>> model_strength; // For strength aggregation rule
            vector<int> model_prediction; // For majority voting rule
            vector<double> model_ndf;
            for(int mi = 0; mi < nb_models; ++mi) {
                proj = models[mi].projection(nc);
                auto m = std::size(proj.second);
                rdf =  m / double(std::size(nc));
                if(min_dfr_pct > rdf)
                    min_dfr_pct = rdf;
                if(max_dfr_pct < rdf)
                    max_dfr_pct = rdf;
                avg_dfr_pct += rdf;

                if(min_dfr > m)
                    min_dfr = m;
                if(max_dfr < m)
                    max_dfr = m;
                avg_dfr += m;
                
                decltype(nc) v(size(nc)+size(proj.second));
                decltype(v)::iterator it;
                it = std::set_difference(begin(nc), end(nc), begin(proj.second), end(proj.second), begin(v));
                v.resize(it-begin(v));

                auto non_disc_features = int(size(v));
                model_ndf.push_back(non_disc_features);
                pred_0 = 0.;
                pred_1 = 0.;
                for(const auto& k: proj.first) {
                    r = size(models[mi].intersection_family[k.first]) / double(non_disc_features);
                    pred_0 += r * models[mi].e_intrinsic_strength[0][k.first];
                    pred_1 += r * models[mi].e_intrinsic_strength[1][k.first];
                   
                }


                pred = normalize_prediction(pred_0, pred_1, delta, 0, 0); //avg_diff_bad_0 / (i+1), avg_diff_bad_1 / (i+1));
                model_strength.push_back(pred);
                prediction = prediction_rule(pred, rdf, gamma, eta0, eta1, bar_eta0, bar_eta1, l0, l1, bias, gen);
                //cerr << "PREDICTION M" << mi << " " << std::setprecision(15) << indexes[i] << " " << o << " " <<  prediction << " " << pred_1 << " " << pred_0 << std::endl;
                //pred_outfile << std::setprecision(15) << indexes[i] << " " << o << " " <<  prediction << " " << pred_1 << " " << pred_0 << std::endl;

                if (check_if_in_cb) {
                    auto index_case = std::find(begin(models[mi].cases), end(models[mi].cases), nc);
                    
                    if(index_case != end(models[mi].cases)) {
                        already_in_cb++;
                        auto index = std::distance(begin(models[mi].cases), index_case);
                        if(models[mi].outcomes[index] == o){
                            already_in_cb_good++;
                        }
                        //cerr << "Already in case base " << models[m].outcomes[index] << " " << o << endl;
                        prediction = models[mi].outcomes[index];
                    }
                }
                model_prediction.push_back(prediction);
            }

            // CALCULATE AVERAGE PREDICTION
            // TEST 1 - HARD MAJORITY RULE
            std::vector<int> votes = {0, 0};
            for(auto m = 0; m < nb_models; ++m) {
                auto c = model_prediction[m];
                //cerr << "M" << m << " P=" << c << endl;
                votes[c]++;
            }

            auto max = votes[0];
            auto max_i = 0;
            for(auto i = 0; i < size(votes); ++i) {
                if(votes[i] > max) {
                    max_i = i;
                    max = votes[i];
                }
            }
            prediction = max_i;

            cerr << "FINAL PREDICTION: " << prediction << " | " << o << " | " << model_prediction << " | " << votes << endl;

            // TEST 2 - SOFT RULE

            // UPDATE THE MODELS
            avr_good += 1 - abs(o - prediction);
            if(prediction == 1)
                if(o - prediction != 0) {
                    fp += 1;
                    avg_diff_bad_1 += std::get<1>(pred) - std::get<0>(pred);
                    avg_diff_bad_1_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                }
                else {
                    tp += 1;
                    avg_diff_good_1 += std::get<1>(pred) - std::get<0>(pred);
                    avg_diff_good_1_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                }
            if(prediction == 0)
                if (o - prediction != 0) {
                    fn += 1;
                    avg_diff_bad_0 += std::get<1>(pred) - std::get<0>(pred);
                    avg_diff_bad_0_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                }
                else {
                    tn += 1;
                    avg_diff_good_0 += std::get<1>(pred) - std::get<0>(pred);
                    avg_diff_good_0_pct += (std::get<1>(pred) - std::get<0>(pred)) / (std::get<1>(pred) + std::get<0>(pred));
                }

            ///*
            if (online) {
              for(auto m = 0; m < nb_models; ++m) {
                  prediction = model_prediction[m];
                  if(abs(o - prediction) != 0) {
                      for(const auto& k: proj.first) {
                          if(prediction == 1) {
                              r = size(models[m].intersection_family[k.first]) / double(model_ndf[m]);
                              models[m].e_intrinsic_strength[0][k.first] += r * abs(models[m].e_intrinsic_strength[0][k.first] - models[m].e_intrinsic_strength[1][k.first]) / size(proj.first); //abs(std::get<1>(pred) - std::get<0>(pred)) / size(proj.first);
                              models[m].e_intrinsic_strength[1][k.first] -= r * abs(models[m].e_intrinsic_strength[0][k.first] - models[m].e_intrinsic_strength[1][k.first]) / size(proj.first);//abs(std::get<1>(pred) - std::get<0>(pred)) / size(proj.first);
                          } else {
                              r = size(models[m].intersection_family[k.first]) / double(model_ndf[m]);
                              models[m].e_intrinsic_strength[0][k.first] -= r * abs(models[m].e_intrinsic_strength[0][k.first] - models[m].e_intrinsic_strength[1][k.first]) / size(proj.first);//abs(std::get<1>(pred) - std::get<0>(pred)) / size(proj.first);
                              models[m].e_intrinsic_strength[1][k.first] += r * abs(models[m].e_intrinsic_strength[0][k.first] - models[m].e_intrinsic_strength[1][k.first]) / size(proj.first);//abs(std::get<1>(pred) - std::get<0>(pred)) / size(proj.first);
                          }
                      }
                  }
              }
            }
            //*/
           
            //models[m].display();
            auto end_iteration = std::chrono::steady_clock::now();
            auto diff = end_iteration - start_iteration;
            auto iteration_time = std::chrono::duration<double, std::ratio<1, 1>>(diff).count();
            total_time += iteration_time;
            accuracy = avr_good / (j+1);
            
            if(!sample_out || i > limit_examples) {
                auto c = j;
                if(keep_offset) {
                    c = i;
                }
                cout << std::fixed << c << " " 
                     << o << " " 
                     << prediction << " " 
                     << avr_good << " " 
                     << accuracy << " " 
                     << std::setprecision(15)
                     << std::get<1>(pred) << " " 
                     << std::get<0>(pred) << " "
                     << rdf << " " 
                     << pred_0 + rdf << " " 
                     << iteration_time << " " 
                     << total_time << " " 
                     << std::get<1>(pred) - std::get<0>(pred) << " "
                     << avg_diff_bad_1 / (j+1) << " "
                     << avg_diff_bad_0 / (j+1) << " "
                     << min_toward_1 << " "
                     << min_toward_0 << " "
                     << std::get<1>(pred) - std::get<0>(pred) << " " 
                     << std::max(std::get<1>(pred), std::get<0>(pred)) / (abs(std::get<1>(pred)) + abs(std::get<0>(pred)))
                     << endl;
                ++j;
                log << iteration_time << " , " 
                    << total_time << " , " 
                    << std::size(nc) << " , " 
                    << o << " , " 
                    << prediction << " , " 
                    << avr_good << " , " 
                    << accuracy << " , " 
                    << std::get<1>(pred) << " , " 
                    << std::get<0>(pred) << " , "
                    << std::size(proj.first) << " , "
                    << std::size(proj.second) << " , "
                    << rdf << " , " 
                    << std::to_string(tp) << " , "
                    << std::to_string(fp) << " , "
                    << std::to_string(fn) << " , "
                    << std::to_string(tn) << " , "
                    << std::to_string(avg_diff_good_1 / tp) << " , "
                    << std::to_string(avg_diff_good_0 / tn) << " , "
                    << std::to_string(avg_diff_bad_1 / fp) << " , "
                    << std::to_string(avg_diff_bad_0 / fn) << " , "
                    << std::to_string(100 * avg_diff_good_1_pct / tp) << " , "
                    << std::to_string(100 * avg_diff_good_0_pct / tn) << " , "
                    << std::to_string(100 * avg_diff_bad_1_pct / fp) << " , "
                    << std::to_string(100 * avg_diff_bad_0_pct / fn) 
                    << endl;
            }
        }
        log.close();
        std::cerr << "# Already in case-base: " << already_in_cb << " " << already_in_cb_good / double(already_in_cb) << std::endl;
        end_time = std::chrono::steady_clock::now();
        diff = end_time - start_time;
        time = std::chrono::duration<double, std::ratio<1, 1>>(diff).count();
        log_run += std::to_string(time) + " , " + std::to_string(n_cases - limit_examples + 1) + " , ";

        diff = end_time - start_global_time;
        time = std::chrono::duration<double, std::ratio<1, 1>>(diff).count();
        log_run += std::to_string(time) + " , "
                  + std::to_string(min_dfr) + " , "
                  + std::to_string(max_dfr) + " , "
                  + std::to_string(avg_dfr / (limit_examples - starting_case)) + " , "
                  + std::to_string(100 * min_dfr_pct) + " , "
                  + std::to_string(100 * max_dfr_pct) + " , "
                  + std::to_string(100 * avg_dfr_pct / (limit_examples - starting_case)) + " , " 
                  + std::to_string(accuracy) + " , "
                  + std::to_string(tp) + " , "
                  + std::to_string(fp) + " , "
                  + std::to_string(fn) + " , "
                  + std::to_string(tn) + " , "
                  + std::to_string(tp / double(tp + fn))  + " , "
                  + std::to_string(tn / double(tn + fp))  + " , "
                  + std::to_string(tp / double(tp + fp))  + " , "
                  + std::to_string(tn / double(tn + fn))  + " , "
                  + std::to_string(fn / double(fn + tp))  + " , "
                  + std::to_string(fp / double(fp + tn))  + " , "
                  + std::to_string(fp / double(fp + tp))  + " , "
                  + std::to_string(fn / double(fn + tn))  + " , "
                  + std::to_string(2*tp / double(2*tp + fp + fn))  + " , "
                  + std::to_string(((long long int)(tp * tn) - (fp * fn)) / sqrt((long long int)(tp + fp)*(tp + fn)*(tn + fp)*(tn + fn)))  + " , "
                  + std::to_string(avg_diff_good_1 / tp) + " , "
                  + std::to_string(avg_diff_good_0 / tn) + " , "
                  + std::to_string(avg_diff_bad_1 / fp) + " , "
                  + std::to_string(avg_diff_bad_0 / fn) + " , "
                  + std::to_string(100 * avg_diff_good_1_pct / tp) + " , "
                  + std::to_string(100 * avg_diff_good_0_pct / tn) + " , "
                  + std::to_string(100 * avg_diff_bad_1_pct / fp) + " , "
                  + std::to_string(100 * avg_diff_bad_0_pct / fn) + " , "
                  + std::to_string(already_in_cb_good / double(already_in_cb)) + " , ";
        cerr << ((long long int)(tp * tn) - (fp * fn)) / sqrt((long long int)(tp + fp)*(tp + fn)*(tn + fp)*(tn + fn)) << endl;
        log.open(log_file_name("hcbr", -1), std::fstream::in | std::fstream::out | std::fstream::app);
        log << log_run << std::endl;
        log.close();

        cerr << "Accuracy: " << (tp + tn) << "/" << (tp + tn + fn + fp) << " = " << (tp + tn) / double(tp + tn + fn + fp) << endl;
        cerr << "Average error toward 0: " << avg_diff_bad_0 / (limit_examples - starting_case + 1) << " (" << fn << ")" << endl;
        cerr << "Average error toward 1: " << avg_diff_bad_1 / (limit_examples - starting_case + 1) << " (" << fp << ")" << endl;
        //cerr << "Prev: " << prev / (limit_examples - starting_case) << " (error: " << double(fn) / (limit_examples - starting_case) << ") Offset: " <<  offset_0 << " " << offset_1 << endl;
        cerr << "Ratio error 1 : " << double(fp) / (fn + fp) << endl;

        cerr << "-----------------" << endl;
        cerr << "- " << std::fixed << tp << " - " << fp << " - " << endl;
        cerr << "-----------------" << endl;
        cerr << "- " << std::fixed << fn << " - " << tn << " - " << endl;
        cerr << "-----------------" << endl;
    }

}
 