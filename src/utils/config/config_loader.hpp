//
// Created by Aiziboy on 2025/10/1.
//

#ifndef UNTITLED1_CONFIG_LOADER_HPP
#define UNTITLED1_CONFIG_LOADER_HPP

#include "config.hpp"
#include <string>


class config_loader {
public:
    explicit config_loader(const std::string& path);
    const AppConfigRoot& get() const;
    void reload(); // 支持热加载
private:
    AppConfigRoot config_;
};


#endif //UNTITLED1_CONFIG_LOADER_HPP