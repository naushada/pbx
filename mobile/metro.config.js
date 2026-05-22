const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');

/**
 * Metro bundler configuration.
 * https://reactnative.dev/docs/metro
 */
module.exports = mergeConfig(getDefaultConfig(__dirname), {});
